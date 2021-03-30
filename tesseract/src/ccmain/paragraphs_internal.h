/**********************************************************************
 * File:        paragraphs_internal.h
 * Description: Paragraph Detection internal data structures.
 * Author:      David Eger
 *
 * (C) Copyright 2011, Google Inc.
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 ** http://www.apache.org/licenses/LICENSE-2.0
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 *
 **********************************************************************/

#ifndef TESSERACT_CCMAIN_PARAGRAPHS_INTERNAL_H_
#define TESSERACT_CCMAIN_PARAGRAPHS_INTERNAL_H_

#include "paragraphs.h"
#include <tesseract/publictypes.h>        // for ParagraphJustification

// NO CODE OUTSIDE OF paragraphs.cpp AND TESTS SHOULD NEED TO ACCESS
// DATA STRUCTURES OR FUNCTIONS IN THIS FILE.

namespace tesseract {

class UNICHARSET;
class WERD_CHOICE;

// Return whether the given word is likely to be a list item start word.
TESS_API
bool AsciiLikelyListItem(const STRING &word);

// Return the first Unicode Codepoint from werd[pos].
int UnicodeFor(const UNICHARSET *u, const WERD_CHOICE *werd, int pos);

// Set right word attributes given either a unicharset and werd or a utf8
// string.
TESS_API
void RightWordAttributes(const UNICHARSET *unicharset, const WERD_CHOICE *werd,
                         const STRING &utf8,
                         bool *is_list, bool *starts_idea, bool *ends_idea);

// Set left word attributes given either a unicharset and werd or a utf8 string.
TESS_API
void LeftWordAttributes(const UNICHARSET *unicharset, const WERD_CHOICE *werd,
                        const STRING &utf8,
                        bool *is_list, bool *starts_idea, bool *ends_idea);

enum LineType {
  LT_START = 'S',     // First line of a paragraph.
  LT_BODY = 'C',      // Continuation line of a paragraph.
  LT_UNKNOWN = 'U',   // No clues.
  LT_MULTIPLE = 'M',  // Matches for both LT_START and LT_BODY.
};

// The first paragraph in a page of body text is often un-indented.
// This is a typographic convention which is common to indicate either that:
// (1) The paragraph is the continuation of a previous paragraph, or
// (2) The paragraph is the first paragraph in a chapter.
//
// I refer to such paragraphs as "crown"s, and the output of the paragraph
// detection algorithm attempts to give them the same paragraph model as
// the rest of the body text.
//
// Nonetheless, while building hypotheses, it is useful to mark the lines
// of crown paragraphs temporarily as crowns, either aligned left or right.
extern const ParagraphModel *kCrownLeft;
extern const ParagraphModel *kCrownRight;

inline bool StrongModel(const ParagraphModel *model) {
  return model != nullptr && model != kCrownLeft && model != kCrownRight;
}

struct LineHypothesis {
  LineHypothesis() : ty(LT_UNKNOWN), model(nullptr) {}
  LineHypothesis(LineType line_type, const ParagraphModel *m)
      : ty(line_type), model(m) {}
  LineHypothesis(const LineHypothesis &other)
      : ty(other.ty), model(other.model) {}

  // Copy assignment operator.
  LineHypothesis& operator=(const LineHypothesis& other) {
    ty = other.ty;
    model = other.model;
    return *this;
  }

  bool operator==(const LineHypothesis &other) const {
    return ty == other.ty && model == other.model;
  }

  LineType ty;
  const ParagraphModel *model;
};

class ParagraphTheory;  // Forward Declaration

using SetOfModels = GenericVector<const ParagraphModel *>;

// Row Scratch Registers are data generated by the paragraph detection
// algorithm based on a RowInfo input.
class RowScratchRegisters {
 public:
  // We presume row will outlive us.
  void Init(const RowInfo &row);

  LineType GetLineType() const;

  LineType GetLineType(const ParagraphModel *model) const;

  // Mark this as a start line type, sans model.  This is useful for the
  // initial marking of probable body lines or paragraph start lines.
  void SetStartLine();

  // Mark this as a body line type, sans model.  This is useful for the
  // initial marking of probably body lines or paragraph start lines.
  void SetBodyLine();

  // Record that this row fits as a paragraph start line in the given model,
  void AddStartLine(const ParagraphModel *model);
  // Record that this row fits as a paragraph body line in the given model,
  void AddBodyLine(const ParagraphModel *model);

  // Clear all hypotheses about this line.
  void SetUnknown() { hypotheses_.truncate(0); }

  // Append all hypotheses of strong models that match this row as a start.
  void StartHypotheses(SetOfModels *models) const;

  // Append all hypotheses of strong models matching this row.
  void StrongHypotheses(SetOfModels *models) const;

  // Append all hypotheses for this row.
  void NonNullHypotheses(SetOfModels *models) const;

  // Discard any hypotheses whose model is not in the given list.
  void DiscardNonMatchingHypotheses(const SetOfModels &models);

  // If we have only one hypothesis and that is that this line is a paragraph
  // start line of a certain model, return that model.  Else return nullptr.
  const ParagraphModel *UniqueStartHypothesis() const;

  // If we have only one hypothesis and that is that this line is a paragraph
  // body line of a certain model, return that model.  Else return nullptr.
  const ParagraphModel *UniqueBodyHypothesis() const;

  // Return the indentation for the side opposite of the aligned side.
  int OffsideIndent(tesseract::ParagraphJustification just) const {
    switch (just) {
      case tesseract::JUSTIFICATION_RIGHT: return lindent_;
      case tesseract::JUSTIFICATION_LEFT: return rindent_;
      default: return lindent_ > rindent_ ? lindent_ : rindent_;
    }
  }

  // Return the indentation for the side the text is aligned to.
  int AlignsideIndent(tesseract::ParagraphJustification just) const {
    switch (just) {
      case tesseract::JUSTIFICATION_RIGHT: return rindent_;
      case tesseract::JUSTIFICATION_LEFT: return lindent_;
      default: return lindent_ > rindent_ ? lindent_ : rindent_;
    }
  }

  // Append header fields to a vector of row headings.
  static void AppendDebugHeaderFields(std::vector<STRING> *header);

  // Append data for this row to a vector of debug strings.
  void AppendDebugInfo(const ParagraphTheory &theory,
                       std::vector<STRING> *dbg) const;

  const RowInfo *ri_;

  // These four constants form a horizontal box model for the white space
  // on the edges of each line.  At each point in the algorithm, the following
  // shall hold:
  //   ri_->pix_ldistance = lmargin_ + lindent_
  //   ri_->pix_rdistance = rindent_ + rmargin_
  int lmargin_;
  int lindent_;
  int rindent_;
  int rmargin_;

 private:
  // Hypotheses of either LT_START or LT_BODY
  GenericVector<LineHypothesis> hypotheses_;
};

// A collection of convenience functions for wrapping the set of
// Paragraph Models we believe correctly model the paragraphs in the image.
class ParagraphTheory {
 public:
  // We presume models will outlive us, and that models will take ownership
  // of any ParagraphModel *'s we add.
  explicit ParagraphTheory(std::vector<ParagraphModel *> *models)
      : models_(models) {}
  std::vector<ParagraphModel *> &models() { return *models_; }
  const std::vector<ParagraphModel *> &models() const { return *models_; }

  // Return an existing model if one that is Comparable() can be found.
  // Else, allocate a new copy of model to save and return a pointer to it.
  const ParagraphModel *AddModel(const ParagraphModel &model);

  // Discard any models we've made that are not in the list of used models.
  void DiscardUnusedModels(const SetOfModels &used_models);

  // Return the set of all non-centered models.
  void NonCenteredModels(SetOfModels *models);

  // If any of the non-centered paragraph models we know about fit
  // rows[start, end), return it.  Else nullptr.
  const ParagraphModel *Fits(const GenericVector<RowScratchRegisters> *rows,
                             int start, int end) const;

  int IndexOf(const ParagraphModel *model) const;

 private:
  std::vector<ParagraphModel *> *models_;
  GenericVector<ParagraphModel *> models_we_added_;
};

bool ValidFirstLine(const GenericVector<RowScratchRegisters> *rows,
                    int row, const ParagraphModel *model);
bool ValidBodyLine(const GenericVector<RowScratchRegisters> *rows,
                   int row, const ParagraphModel *model);
bool CrownCompatible(const GenericVector<RowScratchRegisters> *rows,
                     int a, int b, const ParagraphModel *model);

// A class for smearing Paragraph Model hypotheses to surrounding rows.
// The idea here is that StrongEvidenceClassify first marks only exceedingly
// obvious start and body rows and constructs models of them.  Thereafter,
// we may have left over unmarked lines (mostly end-of-paragraph lines) which
// were too short to have much confidence about, but which fit the models we've
// constructed perfectly and which we ought to mark.  This class is used to
// "smear" our models over the text.
class ParagraphModelSmearer {
 public:
  ParagraphModelSmearer(GenericVector<RowScratchRegisters> *rows,
                        int row_start, int row_end,
                        ParagraphTheory *theory);

  // Smear forward paragraph models from existing row markings to subsequent
  // text lines if they fit, and mark any thereafter still unmodeled rows
  // with any model in the theory that fits them.
  void Smear();

 private:
  // Record in open_models_ for rows [start_row, end_row) the list of models
  // currently open at each row.
  // A model is still open in a row if some previous row has said model as a
  // start hypothesis, and all rows since (including this row) would fit as
  // either a body or start line in that model.
  void CalculateOpenModels(int row_start, int row_end);

  SetOfModels &OpenModels(int row) {
    return open_models_[row - row_start_ + 1];
  }

  ParagraphTheory *theory_;
  GenericVector<RowScratchRegisters> *rows_;
  int row_start_;
  int row_end_;

  // open_models_ corresponds to rows[start_row_ - 1, end_row_]
  //
  // open_models_:  Contains models which there was an active (open) paragraph
  //                as of the previous line and for which the left and right
  //                indents admit the possibility that this text line continues
  //                to fit the same model.
  // TODO(eger): Think about whether we can get rid of "Open" models and just
  //   use the current hypotheses on RowScratchRegisters.
  std::vector<SetOfModels> open_models_;
};

// Clear all hypotheses about lines [start, end) and reset the margins to the
// percentile (0..100) value of the left and right row edges for this run of
// rows.
void RecomputeMarginsAndClearHypotheses(
    GenericVector<RowScratchRegisters> *rows, int start, int end,
    int percentile);

// Return the median inter-word space in rows[row_start, row_end).
int InterwordSpace(const GenericVector<RowScratchRegisters> &rows,
                   int row_start, int row_end);

// Return whether the first word on the after line can fit in the space at
// the end of the before line (knowing which way the text is aligned and read).
bool FirstWordWouldHaveFit(const RowScratchRegisters &before,
                           const RowScratchRegisters &after,
                           tesseract::ParagraphJustification justification);

// Return whether the first word on the after line can fit in the space at
// the end of the before line (not knowing the text alignment).
bool FirstWordWouldHaveFit(const RowScratchRegisters &before,
                           const RowScratchRegisters &after);

// Do rows[start, end) form a single instance of the given paragraph model?
bool RowsFitModel(const GenericVector<RowScratchRegisters> *rows,
                  int start, int end, const ParagraphModel *model);

// Given a set of row_owners pointing to PARAs or nullptr (no paragraph known),
// normalize each row_owner to point to an actual PARA, and output the
// paragraphs in order onto paragraphs.
void CanonicalizeDetectionResults(
    GenericVector<PARA *> *row_owners,
    PARA_LIST *paragraphs);

}  // namespace

#endif  // TESSERACT_CCMAIN_PARAGRAPHS_INTERNAL_H_
