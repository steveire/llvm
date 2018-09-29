//===-- LineEditor.cpp - line editor --------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/LineEditor/LineEditor.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Config/config.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <cstdio>

#undef HAVE_LIBEDIT

#ifdef HAVE_LIBEDIT
#include <histedit.h>
#endif

#include "replxx.hxx"
#include <regex>

using Replxx = replxx::Replxx;

using namespace llvm;

std::string LineEditor::getDefaultHistoryPath(StringRef ProgName) {
  SmallString<32> Path;
  if (sys::path::home_directory(Path)) {
    sys::path::append(Path, "." + ProgName + "-history");
    return Path.str();
  }
  return std::string();
}

LineEditor::CompleterConcept::~CompleterConcept() {}
LineEditor::ListCompleterConcept::~ListCompleterConcept() {}

std::string LineEditor::ListCompleterConcept::getCommonPrefix(
    const std::vector<Completion> &Comps) {
  assert(!Comps.empty());

  std::string CommonPrefix = Comps[0].TypedText;
  for (std::vector<Completion>::const_iterator I = Comps.begin() + 1,
                                               E = Comps.end();
       I != E; ++I) {
    size_t Len = std::min(CommonPrefix.size(), I->TypedText.size());
    size_t CommonLen = 0;
    for (; CommonLen != Len; ++CommonLen) {
      if (CommonPrefix[CommonLen] != I->TypedText[CommonLen])
        break;
    }
    CommonPrefix.resize(CommonLen);
  }
  return CommonPrefix;
}

LineEditor::CompletionAction
LineEditor::ListCompleterConcept::complete(StringRef Buffer, size_t Pos) const {
  CompletionAction Action;
  std::vector<Completion> Comps = getCompletions(Buffer, Pos);
  if (Comps.empty()) {
    Action.Kind = CompletionAction::AK_ShowCompletions;
    return Action;
  }

  std::string CommonPrefix = getCommonPrefix(Comps);

  // If the common prefix is non-empty we can simply insert it. If there is a
  // single completion, this will insert the full completion. If there is more
  // than one, this might be enough information to jog the user's memory but if
  // not the user can also hit tab again to see the completions because the
  // common prefix will then be empty.
  if (CommonPrefix.empty()) {
    Action.Kind = CompletionAction::AK_ShowCompletions;
    for (std::vector<Completion>::iterator I = Comps.begin(), E = Comps.end();
         I != E; ++I)
      Action.Completions.push_back(I->DisplayText);
  } else {
    Action.Kind = CompletionAction::AK_Insert;
    Action.Text = CommonPrefix;
  }

  return Action;
}

LineEditor::CompletionAction LineEditor::getCompletionAction(StringRef Buffer,
                                                             size_t Pos) const {
  if (!Completer) {
    CompletionAction Action;
    Action.Kind = CompletionAction::AK_ShowCompletions;
    return Action;
  }

  return Completer->complete(Buffer, Pos);
}

#ifdef HAVE_LIBEDIT

// libedit-based implementation.

struct LineEditor::InternalData {
  LineEditor *LE;

  History *Hist;
  EditLine *EL;

  unsigned PrevCount;
  std::string ContinuationOutput;

  FILE *Out;
};

namespace {

const char *ElGetPromptFn(EditLine *EL) {
  LineEditor::InternalData *Data;
  if (el_get(EL, EL_CLIENTDATA, &Data) == 0)
    return Data->LE->getPrompt().c_str();
  return "> ";
}

// Handles tab completion.
//
// This function is really horrible. But since the alternative is to get into
// the line editor business, here we are.
unsigned char ElCompletionFn(EditLine *EL, int ch) {
  LineEditor::InternalData *Data;
  if (el_get(EL, EL_CLIENTDATA, &Data) == 0) {
    if (!Data->ContinuationOutput.empty()) {
      // This is the continuation of the AK_ShowCompletions branch below.
      FILE *Out = Data->Out;

      // Print the required output (see below).
      ::fwrite(Data->ContinuationOutput.c_str(),
               Data->ContinuationOutput.size(), 1, Out);

      // Push a sequence of Ctrl-B characters to move the cursor back to its
      // original position.
      std::string Prevs(Data->PrevCount, '\02');
      ::el_push(EL, const_cast<char *>(Prevs.c_str()));

      Data->ContinuationOutput.clear();

      return CC_REFRESH;
    }

    const LineInfo *LI = ::el_line(EL);
    LineEditor::CompletionAction Action = Data->LE->getCompletionAction(
        StringRef(LI->buffer, LI->lastchar - LI->buffer),
        LI->cursor - LI->buffer);
    switch (Action.Kind) {
    case LineEditor::CompletionAction::AK_Insert:
      ::el_insertstr(EL, Action.Text.c_str());
      return CC_REFRESH;

    case LineEditor::CompletionAction::AK_ShowCompletions:
      if (Action.Completions.empty()) {
        return CC_REFRESH_BEEP;
      } else {
        // Push a Ctrl-E and a tab. The Ctrl-E causes libedit to move the cursor
        // to the end of the line, so that when we emit a newline we will be on
        // a new blank line. The tab causes libedit to call this function again
        // after moving the cursor. There doesn't seem to be anything we can do
        // from here to cause libedit to move the cursor immediately. This will
        // break horribly if the user has rebound their keys, so for now we do
        // not permit user rebinding.
        ::el_push(EL, const_cast<char *>("\05\t"));

        // This assembles the output for the continuation block above.
        raw_string_ostream OS(Data->ContinuationOutput);

        // Move cursor to a blank line.
        OS << "\n";

        // Emit the completions.
        for (std::vector<std::string>::iterator I = Action.Completions.begin(),
                                                E = Action.Completions.end();
             I != E; ++I) {
          OS << *I << "\n";
        }

        // Fool libedit into thinking nothing has changed. Reprint its prompt
        // and the user input. Note that the cursor will remain at the end of
        // the line after this.
        OS << Data->LE->getPrompt()
           << StringRef(LI->buffer, LI->lastchar - LI->buffer);

        // This is the number of characters we need to tell libedit to go back:
        // the distance between end of line and the original cursor position.
        Data->PrevCount = LI->lastchar - LI->cursor;

        return CC_REFRESH;
      }
    }
  }
  return CC_ERROR;
}

} // end anonymous namespace

LineEditor::LineEditor(StringRef ProgName, StringRef HistoryPath, FILE *In,
                       FILE *Out, FILE *Err)
    : Prompt((ProgName + "> ").str()), HistoryPath(HistoryPath),
      Data(new InternalData) {
  if (HistoryPath.empty())
    this->HistoryPath = getDefaultHistoryPath(ProgName);

  Data->LE = this;
  Data->Out = Out;

  Data->Hist = ::history_init();
  assert(Data->Hist);

  Data->EL = ::el_init(ProgName.str().c_str(), In, Out, Err);
  assert(Data->EL);

  // https://www.reddit.com/r/cpp/comments/825b7w/replxx_readlinelibedit_replacement_library_now/

  ::el_set(Data->EL, EL_PROMPT, ElGetPromptFn);
  ::el_set(Data->EL, EL_EDITOR, "emacs");
  ::el_set(Data->EL, EL_HIST, history, Data->Hist);
  ::el_set(Data->EL, EL_ADDFN, "tab_complete", "Tab completion function",
           ElCompletionFn);
  ::el_set(Data->EL, EL_BIND, "\t", "tab_complete", NULL);
  ::el_set(Data->EL, EL_BIND, "^r", "em-inc-search-prev",
           NULL); // Cycle through backwards search, entering string
  ::el_set(Data->EL, EL_BIND, "^w", "ed-delete-prev-word",
           NULL); // Delete previous word, behave like bash does.
  ::el_set(Data->EL, EL_BIND, "\033[3~", "ed-delete-next-char",
           NULL); // Fix the delete key.
  ::el_set(Data->EL, EL_BIND, "\\e[1;5C", "em-next-word", NULL);
  ::el_set(Data->EL, EL_BIND, "\\e[1;5D", "ed-prev-word", NULL);
  ::el_set(Data->EL, EL_CLIENTDATA, Data.get());

  HistEvent HE;
  ::history(Data->Hist, &HE, H_SETSIZE, 800);
  ::history(Data->Hist, &HE, H_SETUNIQUE, 1);
  loadHistory();
}

LineEditor::~LineEditor() {
  saveHistory();

  ::history_end(Data->Hist);
  ::el_end(Data->EL);
  ::fwrite("\n", 1, 1, Data->Out);
}

void LineEditor::saveHistory() {
  if (!HistoryPath.empty()) {
    HistEvent HE;
    ::history(Data->Hist, &HE, H_SAVE, HistoryPath.c_str());
  }
}

void LineEditor::loadHistory() {
  if (!HistoryPath.empty()) {
    HistEvent HE;
    ::history(Data->Hist, &HE, H_LOAD, HistoryPath.c_str());
  }
}

Optional<std::string> LineEditor::readLine() const {
  // Call el_gets to prompt the user and read the user's input.
  int LineLen = 0;
  const char *Line = ::el_gets(Data->EL, &LineLen);

  // Either of these may mean end-of-file.
  if (!Line || LineLen == 0)
    return Optional<std::string>();

  // Strip any newlines off the end of the string.
  while (LineLen > 0 &&
         (Line[LineLen - 1] == '\n' || Line[LineLen - 1] == '\r'))
    --LineLen;

  HistEvent HE;
  if (LineLen > 0)
    ::history(Data->Hist, &HE, H_ENTER, Line);

  return std::string(Line, LineLen);
}

#else // HAVE_LIBEDIT


struct LineEditor::InternalData {
  LineEditor *LE;

  Replxx rx;

  FILE *Out;

  using cl = Replxx::Color;

  std::vector<std::pair<std::string, cl>> regex_color {

    // commands
    {"^\\s*help\\b", cl::BRIGHTMAGENTA},
    {"^\\s*quit\\b", cl::BRIGHTMAGENTA},
    {"^\\s*set\\b", cl::BRIGHTMAGENTA},
    {"^\\s*enable\\b", cl::BRIGHTMAGENTA},
    {"^\\s*disable\\b", cl::BRIGHTMAGENTA},
    {"^\\s*match\\b", cl::BRIGHTMAGENTA},
    {"^\\s*let\\b", cl::BRIGHTMAGENTA},
    {"^\\s*m\\b", cl::BRIGHTMAGENTA},
    {"^\\s*l\\b", cl::BRIGHTMAGENTA},
    {"^\\s*q\\b", cl::BRIGHTMAGENTA},

    {"true", cl::YELLOW},
    {"false", cl::YELLOW},
    {"[0-9]+", cl::BLUE},

    // strings
    {"\".*?\"", cl::YELLOW}, // double quotes
    {"\'.*?\'", cl::YELLOW}, // single quotes
  };


};

Replxx::completions_t hook_completion(std::string const& context, int index, void* user_data) {

  if (!context.empty() && context.back() == ',')
    return {};

  auto Data = static_cast<LineEditor::InternalData*>(user_data);

    LineEditor::CompletionAction Action = Data->LE->getCompletionAction(context, context.size());

    switch (Action.Kind) {
    case LineEditor::CompletionAction::AK_Insert: {
      if (!Action.Text.empty()) {
       if (Action.Text.back() == '"') {
        Action.Text.append("\")");
        Action.Text.append(std::string(2, 2));
      }
       if (Action.Text.back() == '(') {
        Action.Text.append(")");
        Action.Text.append(std::string(1, 2));
      }
      }
      return { context.substr(index) + Action.Text };
    }
    case LineEditor::CompletionAction::AK_ShowCompletions: {
      return Action.Completions;
    }
    }
    return {};
}

Replxx::hints_t hook_hint(std::string const& context, int index, Replxx::Color& color, void* user_data) {

  if (!context.empty() && context.back() == ',')
    return {};
  
  auto Data = static_cast<LineEditor::InternalData*>(user_data);

    LineEditor::CompletionAction Action = Data->LE->getCompletionAction(context, context.size());

    switch (Action.Kind) {
    case LineEditor::CompletionAction::AK_Insert:
      return { Action.Text };
    case LineEditor::CompletionAction::AK_ShowCompletions:
    {
      Replxx::completions_t res;
      std::transform(Action.Completions.begin(), Action.Completions.end(), std::back_inserter(res), [&context, index](std::string const& item){
          return item.substr(context.size() - index);
      });
      return res;
    }
    }
    return {};
}

int real_len( std::string const& s ) {
  int len( 0 );
  char m4( 128 + 64 + 32 + 16 );
  char m3( 128 + 64 + 32 );
  char m2( 128 + 64 );
  for ( int i( 0 ); i < static_cast<int>( s.length() ); ++ i, ++ len ) {
    char c( s[i] );
    if ( ( c & m4 ) == m4 ) {
      i += 3;
    } else if ( ( c & m3 ) == m3 ) {
      i += 2;
    } else if ( ( c & m2 ) == m2 ) {
      i += 1;
    }
  }
  return ( len );
}

void hook_color(std::string const& context, Replxx::colors_t& colors, void* user_data) {
  auto Data = static_cast<LineEditor::InternalData*>(user_data);

  // highlight matching regex sequences
  for (auto const& e : Data->regex_color) {
    size_t pos {0};
    std::string str = context;
    std::smatch match;

    while(std::regex_search(str, match, std::regex(e.first))) {
      std::string c {match[0]};
      pos += real_len( match.prefix() );
      int len( real_len( c ) );

      for (int i = 0; i < len; ++i) {
        colors.at(pos + i) = e.second;
      }

      pos += len;
      str = match.suffix();
    }
  }
}


LineEditor::LineEditor(StringRef ProgName, StringRef HistoryPath, FILE *In,
                       FILE *Out, FILE *Err)
    : Prompt((ProgName + "> ").str()), HistoryPath(HistoryPath),
      Data(new InternalData) {
  if (HistoryPath.empty())
    this->HistoryPath = getDefaultHistoryPath(ProgName);

  Data->Out = Out;

  Data->LE = this;

  Data->rx.install_window_change_handler();

  // set the max history size
  Data->rx.set_max_history_size(120);

  // set the max input line size
  Data->rx.set_max_line_size(9999);

  // set the max number of hint rows to show
  Data->rx.set_max_hint_rows(8);

  // set the callbacks
  Data->rx.set_completion_callback(hook_completion, static_cast<void*>(Data.get()));
  Data->rx.set_highlighter_callback(hook_color, static_cast<void*>(Data.get()));
  Data->rx.set_hint_callback(hook_hint, static_cast<void*>(Data.get()));

  loadHistory();
}

LineEditor::~LineEditor() {
  saveHistory();
}

void LineEditor::saveHistory() {
  Data->rx.history_save(this->HistoryPath);
}
void LineEditor::loadHistory() {
  Data->rx.history_load(this->HistoryPath);
}

Optional<std::string> LineEditor::readLine() const {
    char const* cinput{ nullptr };

    do {
      cinput = Data->rx.input("\033[0;32m" + this->Prompt + "\033[0m");
    } while ( ( cinput == nullptr ) && ( errno == EAGAIN ) );

    if (cinput == nullptr) {
      return {};
    }

    std::string input{cinput};

    Data->rx.history_add(input);

    return input;
}

#endif // HAVE_LIBEDIT
