/*
 * Copyright 2016 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <string>

#include <unicode/ubidi.h>
#include <unicode/unistr.h>

#include "fontdiff/diff_job.h"
#include "fontdiff/font_collection.h"
#include "fontdiff/icu_helper.h"
#include "fontdiff/language.h"
#include "fontdiff/line.h"
#include "fontdiff/line_differ.h"
#include "fontdiff/page.h"
#include "fontdiff/paragraph.h"
#include "fontdiff/shaped_text.h"
#include "fontdiff/style.h"

namespace fontdiff {

Paragraph::Paragraph(DiffJob* job,
		     const FontCollection* beforeFonts,
		     const FontCollection* afterFonts)
  : job_(job),
    beforeFonts_(beforeFonts),
    afterFonts_(afterFonts) {
}

Paragraph::~Paragraph() {
  for (auto line : beforeLines_) {
    delete line;
  }

  for (auto line : afterLines_) {
    delete line;
  }

  for (auto s : beforeRuns_) {
    delete s;
  }
  for (auto s : afterRuns_) {
    delete s;
  }
}

void Paragraph::AppendSpan(const icu::StringPiece& text,
                           const Style* style) {
  std::string s(text.data(), text.length());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n' || s[i] == '\r') {
      s[i] = ' ';
    }
  }
  text_.append(icu::UnicodeString::fromUTF8(s));
  Paragraph::Span span;
  span.limit = text_.length();
  span.style = style;
  spans_.push_back(span);
}

void Paragraph::Layout(FT_F26Dot6 width) {
  UErrorCode err = U_ZERO_ERROR;
  UBiDi* paraBidi = ubidi_openSized(text_.length(), 0, &err);
  CheckUErrorCode(err);
  UBiDi* lineBidi = ubidi_openSized(text_.length(), 0, &err);
  CheckUErrorCode(err);
  ubidi_setPara(paraBidi, text_.getBuffer(), text_.length(),
	        UBIDI_DEFAULT_LTR, NULL, &err);
  CheckUErrorCode(err);
  int32_t start = 0, limit = 1;
  UBiDiLevel bidiLevel = 0;
  while (true) {
    ubidi_getLogicalRun(paraBidi, start, &limit, &bidiLevel);
    if (limit <= start) {
      break;
    }
    ShapeBidiRun(start, limit, bidiLevel);
    start = limit;
  }

  std::vector<int32_t> potentialLineBreaks, lineBreaks;
  FindPotentialLineBreaks(&potentialLineBreaks);
  int32_t lineStart = 0, lastBreakPos = 0;
  for (int32_t breakPos : potentialLineBreaks) {
    FT_F26Dot6 beforeXAdvance = 0, beforeAscender = 0, beforeDescender = 0;
    FT_F26Dot6 afterXAdvance = 0, afterAscender = 0, afterDescender = 0;
    MeasureText(/* before? */ true, lineStart, breakPos,
                &beforeXAdvance, &beforeAscender, &beforeDescender);
    MeasureText(/* before? */ false, lineStart, breakPos,
                &afterXAdvance, &afterAscender, &afterDescender);
    FT_F26Dot6 lineWidth = std::max(beforeXAdvance, afterXAdvance);
    if (lineWidth > width) {
      if (lastBreakPos > 0) {
        lineBreaks.push_back(lastBreakPos);
      }
      lineStart = lastBreakPos;
    }
    lastBreakPos = breakPos;
  }
  lineBreaks.push_back(text_.length());
  start = 0;
  for (int32_t pos : lineBreaks) {
    if (pos >= text_.length()) {
      break;
    }
    AddLine(paraBidi, lineBidi, width, start, pos);
    start = pos;
  }
  AddLine(paraBidi, lineBidi, width, start, text_.length());

  ubidi_close(lineBidi);
  ubidi_close(paraBidi);
}

void Paragraph::AddLine(UBiDi* paraBidi, UBiDi* lineBidi, FT_F26Dot6 width,
			int32_t start, int32_t limit) {
  if (!paraBidi || !lineBidi || start >= limit) {
    return;
  }

  UErrorCode err = U_ZERO_ERROR;
  std::unique_ptr<Line> beforeLine(new Line(width));
  std::unique_ptr<Line> afterLine(new Line(width));
  ubidi_setLine(paraBidi, start, limit, lineBidi, &err);
  CheckUErrorCode(err);

  const int32_t numRuns = ubidi_countRuns(lineBidi, &err);
  CheckUErrorCode(err);
  int32_t runStart = 0, runLength = 0;
  for (int32_t i = 0; i < numRuns; ++i) {
    const UBiDiDirection direction =
        ubidi_getVisualRun(lineBidi, i, &runStart, &runLength);
    AddRunsToLine(/* before? */ true, 
		  start + runStart, start + runStart + runLength,
		  beforeLine.get());
    AddRunsToLine(/* before? */ false, 
		  start + runStart, start + runStart + runLength,
		  afterLine.get());
  }
  if (numRuns > 0) {
    std::vector<DeltaRange> removals, additions;
    bool hasDeltas = FindDeltas(beforeLine.get(), afterLine.get(),
                                &removals, &additions);
    FT_F26Dot6 height = afterLine->GetHeight();
    if (hasDeltas) {
      job_->SetHasDiffs();
      height += beforeLine->GetHeight();
      //beforeLine->SetBackgroundColor(0xffeeee);
      for (const DeltaRange& range : removals) {
        beforeLine->AddHighlight(range.x, range.width, 0xe5e5e5);
        //beforeLine->AddGray(beforeGC, 0xcccccc);
      }
      //afterLine->SetBackgroundColor(0xeeffee);
      for (const DeltaRange& range : additions) {
        //afterLine->AddHighlight(range.x, range.width, 0x99ff99);
      }
    }
    Page* page = job_->GetCurrentPage();
    if (page->GetY() + height >= DiffJob::pageHeight - DiffJob::marginWidth) {
      page = job_->AddPage();
    }
    if (hasDeltas) {
      page->AddLine(beforeLine.release(), DiffJob::marginWidth, page->GetY());
    }
    page->AddLine(afterLine.release(), DiffJob::marginWidth, page->GetY());
  }
}
  
void Paragraph::ShapeBidiRun(
      int32_t start, int32_t limit, UBiDiLevel bidiLevel) {
  limit = std::min(limit, text_.length());
  if (start >= limit) {
    return;
  }
  const size_t spanIndexLimit =
      std::min(spans_.size(), FindSpan(limit - 1) + 1);
  size_t spanIndex = start > 0 ? FindSpan(start) : 0;
  int32_t spanStart = 
    std::max(spanIndex > 0 ? spans_[spanIndex - 1].limit : 0, start);
  while (spanStart < limit && spanIndex < spans_.size()) {
    const Span& span = spans_[spanIndex];
    const int32_t spanLimit = std::min(limit, span.limit);
    ShapeSpan(spanStart, spanLimit, bidiLevel,
	      beforeFonts_, span.style, &beforeRuns_);
    ShapeSpan(spanStart, spanLimit, bidiLevel,
	      afterFonts_, span.style, &afterRuns_);
    spanStart = spans_[spanIndex].limit;
    ++spanIndex;
  }
}

void Paragraph::ShapeSpan(int32_t start, int32_t limit,
			  UBiDiLevel bidiLevel,
			  const FontCollection* fonts, const Style* style,
			  std::vector<ShapedText*>* result) {
  if (start >= limit || !fonts || !style || !result) {
    return;
  }

  hb_unicode_funcs_t* unicodeFuncs = hb_unicode_funcs_get_default();
  int32_t pos = start, last = start;
  const Font* lastFont = NULL;
  hb_script_t lastScript = HB_SCRIPT_INVALID;
  while (pos < limit) {
    const UChar32 curChar = text_.char32At(pos);
    hb_script_t curScript = hb_unicode_script(unicodeFuncs, curChar);
    if (curScript == HB_SCRIPT_COMMON || curScript == HB_SCRIPT_INHERITED) {
      curScript = lastScript;
    }
    const Font* curFont = fonts->FindFont(curChar, style, lastFont);
    if (curFont != lastFont || curScript != lastScript) {
      if (lastFont && last < pos) {
	result->push_back(
	    new ShapedText(text_.getBuffer(), last, pos,
			   bidiLevel, lastScript, lastFont, style));
      }
      lastFont = curFont;
      lastScript = curScript;
      last = pos;
    }
    ++pos;
    if (curChar > 0xFFFF) {
      ++pos;
    }
  }

  if (lastFont && last < limit) {
    result->push_back(
        new ShapedText(text_.getBuffer(), last, limit,
		       bidiLevel, lastScript, lastFont, style));
  }
}

size_t Paragraph::FindSpan(int32_t pos) const {
  size_t lo = 0, hi = spans_.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (pos < spans_[mid].limit) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

size_t Paragraph::FindShapedRun(const std::vector<ShapedText*>& runs,
				int32_t pos) const {
  size_t lo = 0, hi = runs.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (pos < runs[mid]->GetLimit()) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

void Paragraph::FindPotentialLineBreaks(std::vector<int32_t>* breaks) {
  const Language* curLang = NULL;
  int32_t runStart = 0, curLangStart = 0;
  const size_t riLimit = spans_.size();
  for (size_t ri = 0; ri < riLimit; ++ri) {
    const Span& span = spans_[ri];
    const Language* spanLang = span.style->GetLanguage();
    if (spanLang != curLang) {
      FindPotentialLineBreaks(curLangStart, runStart, curLang, breaks);
      curLang = spanLang;
      curLangStart = runStart;
    }
    runStart = span.limit;
  }
  FindPotentialLineBreaks(curLangStart, text_.length(), curLang, breaks);
}

void Paragraph::FindPotentialLineBreaks(int32_t start, int32_t limit,
					const Language* language,
					std::vector<int32_t>* breaks) {
  if (start >= limit || !language || !breaks) {
    return;
  }

  icu::BreakIterator* breaker = language->GetLineBreaker();
  if (breaker) {
    breaker->setText(text_);
    int32_t cur = breaker->following(start > 0 ? start - 1 : 0);
    while (cur != BreakIterator::DONE && cur < limit) {
      breaks->push_back(cur);
      cur = breaker->next();
    }
  }
}

void Paragraph::MeasureText(bool before, int32_t start, int32_t limit,
			    FT_F26Dot6* xAdvance,
			    FT_F26Dot6* ascender,
			    FT_F26Dot6* descender) const {
  *xAdvance = *ascender = *descender = 0;
  const std::vector<ShapedText*>& runs = before ? beforeRuns_ : afterRuns_;
  const size_t riLimit = runs.size();
  for (size_t ri = 0; ri < riLimit; ++ri) {
    ShapedText* run = runs[ri];
    *xAdvance += run->GetXAdvance(start, limit);
    *ascender = std::max(*ascender, run->GetAscender());
    *descender = std::min(*descender, run->GetDescender());
  }
}

void Paragraph::AddRunsToLine(bool before, int32_t start, int32_t limit,
			      Line* line) {
  const std::vector<ShapedText*>& runs = before ? beforeRuns_ : afterRuns_;
  const size_t riLimit = runs.size();
  FT_F26Dot6 x = 0;
  for (size_t ri = 0; ri < riLimit; ++ri) {
    ShapedText* run = runs[ri];
    if (run->IsCovering(start, limit)) {
      line->AddShapedText(run, start, limit);
    }
  }
}

}  // namespace fontdiff
