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

#ifndef FONTDIFF_LINE_H_
#define FONTDIFF_LINE_H_

#include <stdint.h>
#include <vector>

#include <ft2build.h>
#include FT_GLYPH_H

typedef struct _cairo cairo_t;

namespace fontdiff {

class ShapedText;

class Line {
 public:
  Line(FT_F26Dot6 width);
  ~Line();

  FT_F26Dot6 GetWidth() const { return width_; }
  FT_F26Dot6 GetHeight() const { return GetAscender() - GetDescender(); }

  void Render(cairo_t* gc, FT_F26Dot6 x, FT_F26Dot6 y) const;
  void RenderHighlights(cairo_t* gc, FT_F26Dot6 x, FT_F26Dot6 y) const;

  void AddShapedText(const ShapedText* text, int32_t start, int32_t limit);
  void SetBackgroundColor(uint32_t rgb) { backgroundColor_ = rgb; }
  //void SetTextColor(uint32_t rgba) { textColor_ = rgba; }
  void AddHighlight(FT_F26Dot6 x, FT_F26Dot6 width, uint32_t color);
  void AddGray(cairo_t* gc, uint32_t color);

 private:
  struct Run {
    const ShapedText* text;
    FT_F26Dot6 x;
    int32_t start, limit;
  };

  struct Highlight {
    FT_F26Dot6 x;
    FT_F26Dot6 width;
    uint32_t color;
  };

  FT_F26Dot6 GetAscender() const { return ascender_; }
  FT_F26Dot6 GetDescender() const { return descender_; }

  std::vector<Run> runs_;
  std::vector<Highlight> highlights_;
  FT_F26Dot6 width_, xAdvance_, ascender_, descender_;
  uint32_t backgroundColor_;
};


}  // namespace fontdiff

#endif  // FONTDIFF_LINE_H_
