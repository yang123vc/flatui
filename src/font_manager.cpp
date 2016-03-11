// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "precompiled.h"

// Freetype2 header
#include <ft2build.h>
#include FT_FREETYPE_H

// Harfbuzz header
#include <hb.h>
#include <hb-ft.h>
#include <hb-ot.h>

#include "font_manager.h"
#include "fplbase/fpl_common.h"
#include "fplbase/utilities.h"

#ifdef FLATUI_USE_LIBUNIBREAK
#include "linebreak.h"
#endif

using fplbase::LogInfo;
using fplbase::LogError;
using fplbase::Texture;
using mathfu::vec2;
using mathfu::vec2i;
using mathfu::vec4;

namespace flatui {

// The default script used for a layout.
const hb_script_t kDefaultScript = HB_SCRIPT_LATIN;

// Singleton object of FreeType&Harfbuzz.
FT_Library *FontManager::ft_;
hb_buffer_t *FontManager::harfbuzz_buf_;

// Enum that indicates invalid texture atlas index.
const int32_t kInvalidSliceIndex = -1;

// Enumerate words in a specified buffer using line break information generated
// by libunibreak.
class WordEnumerator {
 private:
  // Delete default constructor.
  WordEnumerator() {}

 public:
  // buffer: a buffer contains a linebreak information.
  // Note that the class accesses the given buffer while it's lifetime.
  WordEnumerator(const std::vector<char> &buffer, bool single_line)
      : current_index_(0), current_length_(0), finished_(false) {
    buffer_ = &buffer;
    single_line_ = single_line;
  }

  // Advance the current word index
  // return: true if the buffer has next word. false if the buffer finishes.
  bool Advance() {
    assert(buffer_);
    if (single_line_ && finished_ == false) {
      // For a single line, allow one iteration.
      finished_ = true;
      current_length_ = buffer_->size();
      return true;
    }

    current_index_ += current_length_;
    if (current_index_ >= buffer_->size() || finished_) {
      return false;
    }

    size_t index = current_index_;
    while (index < buffer_->size()) {
      auto word_info = (*buffer_)[index];
      if (word_info == LINEBREAK_MUSTBREAK ||
          word_info == LINEBREAK_ALLOWBREAK) {
        break;
      }
      index++;
    }
    current_length_ = index - current_index_ + 1;
    return true;
  }

  // Check if current word is the last one.
  bool IsLastWord() {
    return current_index_ + current_length_ >= buffer_->size() || finished_;
  }

  // Get an index of the current word in the given text buffer.
  size_t GetCurrentWordIndex() const {
    assert(buffer_);
    return current_index_;
  }

  // Get a length of the current word in the given text buffer.
  size_t GetCurrentWordLength() const {
    assert(buffer_);
    return current_length_;
  }

  // Retrieve if current word must break a line.
  bool CurrentWordMustBreak() {
    assert(buffer_);
    // Check if the first advance is made.
    if (current_index_ + current_length_ == 0) return false;
    return (*buffer_)[current_index_ + current_length_ - 1] ==
           LINEBREAK_MUSTBREAK;
  }

  // Retrieve the word buffer.
  const std::vector<char> *GetBuffer() const { return buffer_; }

 private:
  size_t current_index_;
  size_t current_length_;
  const std::vector<char> *buffer_;
  bool finished_;
  bool single_line_;
};

FontManager::FontManager() {
  // Initialize variables and libraries.
  Initialize();

  // Initialize glyph cache.
  glyph_cache_.reset(new GlyphCache<uint8_t>(
      mathfu::vec2i(kGlyphCacheWidth, kGlyphCacheHeight),
      kGlyphCacheMaxSlices));
}

FontManager::FontManager(const mathfu::vec2i &cache_size, int32_t max_slices) {
  // Initialize variables and libraries.
  Initialize();

  // Initialize glyph cache.
  glyph_cache_.reset(new GlyphCache<uint8_t>(cache_size, max_slices));
}

FontManager::~FontManager() {}

void FontManager::Initialize() {
  // Initialize variables.
  renderer_ = nullptr;
  face_initialized_ = false;
  current_atlas_revision_ = 0;
  current_pass_ = 0;
  script_ = kDefaultScript;
  language_ = kDefaultLanguage;
  layout_direction_ = kTextLayoutDirectionLTR;
  line_height_ = kLineHeightDefault;
  current_font_ = nullptr;

  if (ft_ == nullptr) {
    ft_ = new FT_Library;
    FT_Error err = FT_Init_FreeType(ft_);
    if (err) {
      // Error! Please fix me.
      LogError("Can't initialize freetype. FT_Error:%d\n", err);
      assert(0);
    }
  }

  if (harfbuzz_buf_ == nullptr) {
    // Create a buffer for harfbuzz.
    harfbuzz_buf_ = hb_buffer_create();
  }

#ifdef FLATUI_USE_LIBUNIBREAK
  // Initialize libunibreak
  init_linebreak();
#else
#error libunibreak is required for multiline label support!
#endif
}

void FontManager::Terminate() {
  assert(ft_ != nullptr);
  hb_buffer_destroy(harfbuzz_buf_);
  harfbuzz_buf_ = nullptr;

  FT_Done_FreeType(*ft_);
  ft_ = nullptr;
}

void FontManager::SetRenderer(fplbase::Renderer &renderer) {
  renderer_ = &renderer;

  // Initialize the font atlas texture.
  ExpandAtlasTexture();
}

FontBuffer *FontManager::GetBuffer(const char *text, size_t length,
                                   const FontBufferParameters &parameter) {
  auto buffer = CreateBuffer(text, length, parameter);
  if (buffer == nullptr) {
    // Flush glyph cache & Upload a texture
    FlushAndUpdate();

    // Try to create buffer again.
    buffer = CreateBuffer(text, length, parameter);
    if (buffer == nullptr) {
      LogError("The given text '%s' with ",
               "size:%d does not fit a glyph cache. Try to "
               "increase a cache size or use GetTexture() API ",
               "instead.\n", text, parameter.get_size().y());
    }
  }

  return buffer;
}

FontBuffer *FontManager::CreateBuffer(const char *text, uint32_t length,
                                      const FontBufferParameters &parameters) {
  // Adjust y size if the size selector is set.
  auto ysize = static_cast<int32_t>(parameters.get_font_size());
  auto size = parameters.get_size();
  auto caret_info = parameters.get_caret_info_flag();
  int32_t converted_ysize = ConvertSize(ysize);
  float scale = ysize / static_cast<float>(converted_ysize);
  bool multi_line = parameters.get_multi_line_setting();

  // Check cache if we already have a FontBuffer generated.
  auto it = map_buffers_.find(parameters);
  if (it != map_buffers_.end()) {
    // Update current pass.
    if (current_pass_ != kRenderPass) {
      it->second->set_pass(current_pass_);
    }

    // Update UV of the buffer
    auto ret = UpdateUV(converted_ysize, parameters.get_glyph_flags(),
                        it->second.get());

    // Increment the reference count if the buffer is ref counting buffer.
    if (parameters.get_ref_count_flag()) {
      ret->set_ref_count(ret->get_ref_count() + 1);
    }
    return ret;
  }

  // Otherwise, create new FontBuffer.

  // Set freetype settings.
  current_font_->SetPixelSize(converted_ysize);

  // Create FontBuffer with derived string length.
  std::unique_ptr<FontBuffer> buffer(new FontBuffer(length, caret_info));

  // Retrieve word breaking information using libunibreak.
  if (length) {
    wordbreak_info_.resize(length);
    set_linebreaks_utf8(reinterpret_cast<const utf8_t *>(text), length,
                        language_.c_str(), &wordbreak_info_[0]);
  }
  WordEnumerator word_enum(wordbreak_info_, !multi_line);

  // Initialize font metrics parameters.
  int32_t base_line = current_font_->GetBaseLine(ysize);
  FontMetrics initial_metrics(base_line, 0, base_line, base_line - ysize, 0);

  float pos_start = 0;
  if (layout_direction_ == kTextLayoutDirectionRTL) {
    // In RTL layout, the glyph position start from right.
    pos_start = static_cast<float>(size.x());
  }
  mathfu::vec2 pos(pos_start, 0);

  uint32_t line_width = 0;
  uint32_t max_line_width = parameters.get_line_length();
  uint32_t total_height = ysize;
  bool lastline_must_break = false;
  bool first_character = true;
  auto line_height = ysize * line_height_;

  // Clear temporary buffer holding texture atlas indices.
  std::fill(atlas_indices_.begin(), atlas_indices_.end(), kInvalidSliceIndex);
  auto slices_in_buffer = buffer->get_slices();

  // Find words and layout them.
  while (word_enum.Advance()) {
    if (!multi_line) {
      // Single line text.
      // In this mode, it layouts all string into single line.
      max_line_width = static_cast<uint32_t>(LayoutText(text, length) * scale);
      if (layout_direction_ == kTextLayoutDirectionRTL && size.x() == 0) {
        pos.x() = static_cast<float>(max_line_width / kFreeTypeUnit);
      }
    } else {
      // Multi line text.
      // In this mode, it layouts every single word in the order of the text and
      // performs a line break if either current word exceeds the max line
      // width or indicated a line break must happen due to a line break
      // character etc.
      uint32_t word_width = static_cast<uint32_t>(
          LayoutText(text + word_enum.GetCurrentWordIndex(),
                     word_enum.GetCurrentWordLength()) *
          scale);
      if (lastline_must_break || (line_width + word_width) / kFreeTypeUnit >
                                     static_cast<uint32_t>(size.x())) {
        // Line break.
        buffer->UpdateLine(parameters, layout_direction_,
                           line_width / kFreeTypeUnit, lastline_must_break);
        pos = vec2(pos_start, pos.y() + line_height);
        total_height += static_cast<int32_t>(line_height);
        first_character = lastline_must_break;
        if (size.y() && total_height > static_cast<uint32_t>(size.y()) &&
            !caret_info) {
          // The text size exceeds given size.
          // For now, we just don't render the rest of strings.

          // Cleanup buffer contents.
          hb_buffer_clear_contents(harfbuzz_buf_);
          break;
        }

        line_width = word_width;
        if (line_width > static_cast<uint32_t>(size.x()) * kFreeTypeUnit) {
          std::string s = std::string(&text[word_enum.GetCurrentWordIndex()],
                                      word_enum.GetCurrentWordLength());
          LogInfo(
              "A single word '%s' exceeded the given line width setting.\n"
              "Currently multiline label doesn't support a hyphenation",
              s.c_str());
        }
      } else {
        line_width += word_width;
      }
      // In case of the layout is left/center aligned, max line width is
      // adjusted based on layout results.
      max_line_width = std::max(max_line_width, line_width);
      lastline_must_break = word_enum.CurrentWordMustBreak();
    }

    // Update the first caret position.
    if (caret_info && first_character) {
      buffer->AddCaretPosition(pos + vec2(0, base_line * scale));
      first_character = false;
    }

    // Retrieve layout info.
    uint32_t glyph_count;
    auto glyph_info = hb_buffer_get_glyph_infos(harfbuzz_buf_, &glyph_count);
    auto glyph_pos = hb_buffer_get_glyph_positions(harfbuzz_buf_, &glyph_count);

    auto idx = 0;
    auto idx_advance = 1;
    if (layout_direction_ == kTextLayoutDirectionRTL) {
      idx = glyph_count - 1;
      idx_advance = -1;
    }

    for (size_t i = 0; i < glyph_count; ++i, idx += idx_advance) {
      auto code_point = glyph_info[idx].codepoint;
      if (!code_point) {
        continue;
      }
      auto cache = GetCachedEntry(code_point, converted_ysize,
                                  parameters.get_glyph_flags());
      if (cache == nullptr) {
        // Cleanup buffer contents.
        hb_buffer_clear_contents(harfbuzz_buf_);
        return nullptr;
      }

      auto pos_advance =
          mathfu::vec2(static_cast<float>(glyph_pos[idx].x_advance),
                       static_cast<float>(-glyph_pos[idx].y_advance)) *
          scale / static_cast<float>(kFreeTypeUnit);
      // Advance positions before rendering in RTL.
      if (layout_direction_ == kTextLayoutDirectionRTL) {
        pos -= pos_advance;
      }

      // Register vertices only when the glyph has a size.
      if (cache->get_size().x() && cache->get_size().y()) {
        // Add the code point to the buffer. This information is used when
        // re-fetching UV information when the texture atlas is updated.
        buffer->AddCodepoint(code_point);

        // Calculate internal/external leading value and expand a buffer if
        // necessary.
        auto info = current_font_->GetGlyphInfo(code_point);
        FT_GlyphSlot glyph = info->GetFtFace()->glyph;
        FontMetrics new_metrics;
        if (UpdateMetrics(glyph, initial_metrics, &new_metrics)) {
          initial_metrics = new_metrics;
        }

        // Expand buffer if necessary.
        auto slice = cache->get_pos().z();
        auto slice_idx = atlas_indices_[slice];
        if (slice_idx == kInvalidSliceIndex) {
          // Resize buffers.
          slice_idx = atlas_indices_[slice] = slices_in_buffer->size();
          buffer->ExpandGlyphBuffers(slice_idx + 1);
          slices_in_buffer->push_back(slice);
        }

        auto current_glyph_count = buffer->get_glyph_count();
        // Construct indices array.
        const uint16_t kIndices[] = {0, 1, 2, 1, 3, 2};
        auto indices = buffer->get_indices(slice_idx);
        for (size_t j = 0; j < FPL_ARRAYSIZE(kIndices); ++j) {
          auto index = kIndices[j];
          indices->push_back(
              static_cast<unsigned short>(index + current_glyph_count * 4));
        }

        // Construct intermediate vertices array.
        // The vertices array is update in the render pass with correct
        // glyph size & glyph cache entry information.

        // Update vertices and UVs
        buffer->AddVertices(pos, base_line, scale, *cache);

        // Update references if the buffer is ref counting buffer.
        if (parameters.get_ref_count_flag()) {
          auto row = cache->get_row();
          row->AddRef(buffer.get());
          buffer->AddCacheRowReference(&*row);
        }
      }

      // Advance positions after rendering in LTR.
      if (layout_direction_ == kTextLayoutDirectionLTR) {
        pos += pos_advance;
      }

      // Update caret information if it has been requested.
      bool end_of_line = lastline_must_break == true && i == glyph_count - 1;
      if (caret_info && end_of_line == false) {
        // Is the current glyph a ligature?
        // We are not using hb_ot_layout_get_ligature_carets() as the API barely
        // work with existing fonts.
        // https://bugs.freedesktop.org/show_bug.cgi?id=90962 tracks a request
        // for the issue.
        auto carets = GetCaretPosCount(word_enum, glyph_info,
                                       static_cast<int32_t>(glyph_count),
                                       static_cast<int32_t>(idx));

        auto scaled_offset = cache->get_offset().x() * scale;
        float scaled_base_line = base_line * scale;
        // Add caret points
        for (auto caret = 1; caret <= carets; ++caret) {
          buffer->AddCaretPosition(
              pos + vec2(idx_advance * (scaled_offset - pos_advance.x() +
                                        caret * pos_advance.x() / carets),
                         scaled_base_line));
        }
      }
    }

    // Add word boundary information.
    buffer->AddWordBoundary(parameters);

    // Set buffer revision using glyph cache revision.
    buffer->set_revision(glyph_cache_->get_revision());

    // Cleanup buffer contents.
    hb_buffer_clear_contents(harfbuzz_buf_);
  }

  // Add the last caret.
  if (caret_info) {
    buffer->AddCaretPosition(pos + vec2(0, base_line * scale));
  }

  // Update the last line.
  buffer->UpdateLine(parameters, layout_direction_, line_width / kFreeTypeUnit,
                     true);

  // Setup size.
  buffer->set_size(vec2i(max_line_width / kFreeTypeUnit, total_height));

  // Setup font metrics.
  buffer->set_metrics(initial_metrics);

  // Initialize reference counter.
  buffer->set_ref_count(1);

  // Set current pass.
  if (current_pass_ != kRenderPass) {
    buffer->set_pass(current_pass_);
  }

  // Verify the buffer.
  assert(buffer->Verify());

  // Insert the created entry to the hash map.
  auto insert = map_buffers_.insert(
      std::pair<FontBufferParameters, std::unique_ptr<FontBuffer>>(
          parameters, std::move(buffer)));

  // Set up a back reference from the buffer to the map.
  insert.first->second->set_iterator(insert.first);
  return insert.first->second.get();
}

void FontManager::ReleaseBuffer(FontBuffer *buffer) {
  assert(buffer->get_ref_count() >= 1);
  buffer->set_ref_count(buffer->get_ref_count() - 1);
  if (!buffer->get_ref_count()) {
    // Clear references in the buffer.
    buffer->ReleaseCacheRowReference();

    // Remove an instance of the buffer.
    map_buffers_.erase(buffer->get_iterator());
  }
}

int32_t FontManager::GetCaretPosCount(const WordEnumerator &word_enum,
                                      const hb_glyph_info_t *glyph_info,
                                      int32_t glyph_count, int32_t index) {
  // Retrieve a byte range for the glyph in the wordbreak buffer from harfbuzz
  // buffer.
  auto byte_index = glyph_info[index].cluster;
  auto byte_size = 0;
  auto direction = layout_direction_ == kTextLayoutDirectionLTR ? 1 : -1;

  if (index >= -direction && index < glyph_count - direction) {
    // Has next word. Calculate a difference between them.
    byte_size = glyph_info[index + direction].cluster - byte_index;
  } else {
    // Up until end of the buffer.
    byte_size = static_cast<int>(word_enum.GetCurrentWordLength() - byte_index);
  }

  // Count the number of characters in the given range in the wordbreak bufer.
  auto num_characters = 0;
  for (auto i = 0; i < byte_size; ++i) {
    if (word_enum.GetBuffer()->at(word_enum.GetCurrentWordIndex() + byte_index +
                                  i) != LINEBREAK_INSIDEACHAR) {
      num_characters++;
    }
  }
  return num_characters;
}

FontBuffer *FontManager::UpdateUV(int32_t ysize, GlyphFlags flags,
                                  FontBuffer *buffer) {
  if (buffer->get_revision() != current_atlas_revision_) {
    // Cache revision has been updated.
    // Some referencing glyph cache entries might have been evicted.
    // So we need to check glyph cache entries again while we can still use
    // layout information.

    // Set freetype settings.
    current_font_->SetPixelSize(ysize);

    auto code_points = buffer->get_code_points();
    for (size_t i = 0; i < code_points->size(); ++i) {
      auto code_point = code_points->at(i);
      auto cache = GetCachedEntry(code_point, ysize, flags);
      if (cache == nullptr) {
        return nullptr;
      }

      // Update UV.
      buffer->UpdateUV(static_cast<int32_t>(i), cache->get_uv());

      // Update revision.
      buffer->set_revision(glyph_cache_->get_revision());
    }
  }
  return buffer;
}

FontTexture *FontManager::GetTexture(const char *text, uint32_t length,
                                     float original_ysize) {
  // Round up y size if the size selector is set.
  int32_t ysize = ConvertSize(static_cast<int32_t>(original_ysize));

  // Note: In the API it uses HbFont's fontID (would include multiple fonts)
  // rather than a fontID for single font file. A texture can have multiple font
  // faces so that we need a fontID that represents all fonts used.
  auto parameter =
      FontBufferParameters(current_font_->GetFontId(), flatui::HashId(text),
                           static_cast<float>(ysize), mathfu::kZeros2i,
                           kTextAlignmentLeft, kGlyphFlagsNone, false, false);

  // Check cache if we already have a texture.
  auto it = map_textures_.find(parameter);
  if (it != map_textures_.end()) {
    return it->second.get();
  }

  // Otherwise, create new texture.

  // Set font size.
  current_font_->SetPixelSize(ysize);

  // Layout text.
  auto string_width = LayoutText(text, length) / kFreeTypeUnit;

  // Retrieve layout info.
  uint32_t glyph_count;
  hb_glyph_info_t *glyph_info =
      hb_buffer_get_glyph_infos(harfbuzz_buf_, &glyph_count);
  hb_glyph_position_t *glyph_pos =
      hb_buffer_get_glyph_positions(harfbuzz_buf_, &glyph_count);

  // Calculate texture size. The texture may be expanded later depending on
  // glyph sizes.
  int32_t width = RoundUpToPowerOf2(string_width);
  int32_t height = RoundUpToPowerOf2(ysize);

  // Initialize font metrics parameters.
  int32_t base_line = current_font_->GetBaseLine(ysize);
  FontMetrics initial_metrics(base_line, 0, base_line, base_line - ysize, 0);

  // rasterized image format in FreeType is 8 bit gray scale format.
  std::unique_ptr<uint8_t[]> image(new uint8_t[width * height]);
  memset(image.get(), 0, width * height);

  // TODO: make padding values configurable.
  float kGlyphPadding = 0.0f;
  mathfu::vec2 pos(kGlyphPadding, kGlyphPadding);

  for (size_t i = 0; i < glyph_count; ++i) {
    auto code_point = glyph_info[i].codepoint;
    if (!code_point) continue;

    auto info = current_font_->GetGlyphInfo(code_point);
    FT_GlyphSlot glyph = info->GetFtFace()->glyph;
    FT_Error err =
        FT_Load_Glyph(info->GetFtFace(), info->GetCodepoint(), FT_LOAD_RENDER);

    // Load glyph using harfbuzz layout information.
    // Note that harfbuzz takes care of ligatures.
    if (err) {
      // Error. This could happen typically the loaded font does not support
      // particular glyph.
      LogInfo("Can't load glyph %c FT_Error:%d\n", text[i], err);
      return nullptr;
    }

    // Calculate internal/external leading value and expand a buffer if
    // necessary.
    FontMetrics new_metrics;
    if (UpdateMetrics(glyph, initial_metrics, &new_metrics)) {
      if (new_metrics.total() != initial_metrics.total()) {
        // Expand buffer and update height if necessary.
        if (ExpandBuffer(width, height, initial_metrics, new_metrics, &image)) {
          height = RoundUpToPowerOf2(new_metrics.total());
        }
      }
      initial_metrics = new_metrics;
    }

    if (i == 0 && glyph->bitmap_left < 0) {
      // Slightly shift all text to right.
      pos.x() = static_cast<float>(-glyph->bitmap_left);
    }

    // Copy the texture
    uint32_t y_offset = initial_metrics.base_line() - glyph->bitmap_top;
    for (uint32_t y = 0; y < glyph->bitmap.rows; ++y) {
      memcpy(&image[(static_cast<size_t>(pos.y()) + y + y_offset) * width +
                    static_cast<size_t>(pos.x()) + glyph->bitmap_left],
             &glyph->bitmap.buffer[y * glyph->bitmap.pitch],
             glyph->bitmap.width);
    }

    // Advance positions.
    pos += mathfu::vec2(static_cast<float>(glyph_pos[i].x_advance),
                        static_cast<float>(-glyph_pos[i].y_advance)) /
           static_cast<float>(kFreeTypeUnit);
  }

  // Create new texture.
  FontTexture *tex = new FontTexture();
  tex->LoadFromMemory(image.get(), vec2i(width, height), false);

  // Setup font metrics.
  tex->set_metrics(initial_metrics);

  // Cleanup buffer contents.
  hb_buffer_clear_contents(harfbuzz_buf_);

  // Put to the dic.
  map_textures_[parameter].reset(tex);

  return tex;
}

bool FontManager::ExpandBuffer(int32_t width, int32_t height,
                               const FontMetrics &original_metrics,
                               const FontMetrics &new_metrics,
                               std::unique_ptr<uint8_t[]> *image) {
  // Returns immediately if the size has not been changed.
  if (original_metrics.total() == new_metrics.total()) {
    return false;
  }

  int32_t new_height = RoundUpToPowerOf2(new_metrics.total());
  int32_t internal_leading_change =
      new_metrics.internal_leading() - original_metrics.internal_leading();
  // Internal leading tracks max value of it in rendering glyphs, so we can
  // assume internal_leading_change >= 0.
  assert(internal_leading_change >= 0);

  if (height != new_height) {
    // Reallocate buffer.
    std::unique_ptr<uint8_t[]> old_image = std::move(*image);
    image->reset(new uint8_t[width * new_height]);

    // Copy existing contents.
    memcpy(&(*image)[width * internal_leading_change], old_image.get(),
           width * original_metrics.total());

    // Clear top.
    memset(image->get(), 0, width * internal_leading_change);

    // Clear bottom.
    int32_t clear_offset = original_metrics.total() + internal_leading_change;
    memset(&(*image)[width * clear_offset], 0,
           width * (new_height - clear_offset));
    return true;
  } else {
    if (internal_leading_change > 0) {
      // Move existing contents down and make a space for additional internal
      // leading.
      memcpy(&(*image)[width * internal_leading_change], image->get(),
             width * original_metrics.total());

      // Clear the top
      memset(image->get(), 0, width * internal_leading_change);
    }
    // Bottom doesn't have to be cleared since the part is already clean.
  }

  return false;
}

bool FontManager::Open(const char *font_name) {
  auto it = map_faces_.find(font_name);
  if (it != map_faces_.end()) {
    // The font has been already opened.
    return false;
  }

  // Insert the created entry to the hash map.
  auto insert =
      map_faces_.insert(std::pair<std::string, std::unique_ptr<FaceData>>(
          font_name, std::unique_ptr<FaceData>(new FaceData)));
  auto face = insert.first->second.get();

  // Load the font file of assets.
  if (!fplbase::LoadFile(font_name, &face->font_data_)) {
    LogInfo("Can't load font reource: %s\n", font_name);
    return false;
  }

  // Open the font.
  FT_Error err = FT_New_Memory_Face(
      *ft_, reinterpret_cast<const unsigned char *>(&face->font_data_[0]),
      static_cast<FT_Long>(face->font_data_.size()), 0, &face->face_);
  if (err) {
    // Failed to open font.
    LogInfo("Failed to initialize font:%s FT_Error:%d\n", font_name, err);
    return false;
  }

  face->font_id_ = HashId(font_name);

  // Set first opened font as a default font.
  if (!face_initialized_) {
    current_font_ = HbFont::Open(*face);
    if (current_font_ == nullptr) {
      // Failed to open font.
      LogInfo("Failed to initialize harfbuzz layout information:%s\n",
              font_name);
      face->font_data_.clear();
      FT_Done_Face(face->face_);
      return false;
    }
  }

  face_initialized_ = true;
  return true;
}

bool FontManager::Close(const char *font_name) {
  auto it = map_faces_.find(font_name);
  if (it == map_faces_.end()) {
    return false;
  }

  // Clean up face instance data.
  it->second->Close();

  map_textures_.clear();
  map_buffers_.clear();

  map_faces_.erase(it);

  if (!map_faces_.size()) {
    face_initialized_ = false;
  }

  return true;
}

bool FontManager::SelectFont(const char *font_name) {
  auto it = map_faces_.find(font_name);
  if (it == map_faces_.end()) {
    return false;
  }
  current_font_ = HbFont::Open(*it->second.get());
  return current_font_ != nullptr;
}

bool FontManager::SelectFont(const char *font_names[], int32_t count) {
  if (count == 1) {
    return SelectFont(font_names[0]);
  }

  // Check if the font is already created.
  auto id = HashId(font_names, count);
  current_font_ = HbFont::Open(id);

  if (current_font_ == nullptr) {
    std::vector<FaceData *> v;
    for (auto i = 0; i < count; ++i) {
      auto it = map_faces_.find(font_names[i]);
      if (it == map_faces_.end()) {
        return false;
      }
      v.push_back(it->second.get());
    }
    current_font_ = HbComplexFont::Open(id, &v);
  }
  return current_font_ != nullptr;
}

void FontManager::StartLayoutPass() {
  // Reset pass.
  current_pass_ = 0;
}

void FontManager::UpdatePass(bool start_subpass) {
  // Increment a cycle counter in glyph cache.
  glyph_cache_->Update();

  if (glyph_cache_->get_dirty_state() && current_pass_ <= 0) {
    auto slices = glyph_cache_->get_num_slices();
    for (auto i = 0; i < slices; ++i) {
      auto rect = glyph_cache_->get_dirty_rect(i);
      atlas_textures_[i]->Set(0);
      Texture::UpdateTexture(fplbase::kFormatLuminance, 0, rect.y(),
                             glyph_cache_->get_size().x(), rect.w() - rect.y(),
                             glyph_cache_->get_buffer(i) +
                                 glyph_cache_->get_size().x() * rect.y());
    }
    current_atlas_revision_ = glyph_cache_->get_revision();
    glyph_cache_->set_dirty_state(false);
  }

  if (start_subpass) {
    if (current_pass_ > 0) {
      LogInfo(
          "Multiple subpasses in one rendering pass is not supported. "
          "When this happens, increase the glyph cache size not to "
          "flush the atlas texture multiple times in one rendering "
          "pass.");
    }
    glyph_cache_->Flush();
    current_atlas_revision_ = glyph_cache_->get_revision();
    current_pass_++;
  } else {
    // Reset pass.
    current_pass_ = kRenderPass;
  }
}

uint32_t FontManager::LayoutText(const char *text, size_t length) {
  SetLanguageSettings();
  hb_buffer_set_language(
      harfbuzz_buf_, hb_language_from_string(text, static_cast<int>(length)));

  // Layout the text.
  hb_buffer_add_utf8(harfbuzz_buf_, text, static_cast<unsigned int>(length), 0,
                     static_cast<int>(length));
  hb_shape(current_font_->GetHbFont(), harfbuzz_buf_, nullptr, 0);

  // Retrieve layout info.
  uint32_t glyph_count;
  hb_glyph_position_t *glyph_pos =
      hb_buffer_get_glyph_positions(harfbuzz_buf_, &glyph_count);

  // Retrieve a width of the string.
  uint32_t string_width = 0;
  for (uint32_t i = 0; i < glyph_count; ++i) {
    string_width += glyph_pos[i].x_advance;
  }
  return string_width;
}

bool FontManager::UpdateMetrics(const FT_GlyphSlot g,
                                const FontMetrics &current_metrics,
                                FontMetrics *new_metrics) {
  // Calculate internal/external leading value and expand a buffer if
  // necessary.
  if (g->bitmap_top > current_metrics.ascender() ||
      static_cast<int32_t>(g->bitmap_top - g->bitmap.rows) <
          current_metrics.descender()) {
    *new_metrics = current_metrics;
    new_metrics->set_internal_leading(
        std::max(current_metrics.internal_leading(),
                 g->bitmap_top - current_metrics.ascender()));
    new_metrics->set_external_leading(
        std::min(current_metrics.external_leading(),
                 static_cast<int32_t>(g->bitmap_top - g->bitmap.rows -
                                      current_metrics.descender())));
    new_metrics->set_base_line(new_metrics->internal_leading() +
                               new_metrics->ascender());

    return true;
  }
  return false;
}

void FontManager::SetLocale(const char *locale) {
  if (locale_ == locale) {
    return;
  }
  // Retrieve the language in the locale string.
  std::string language = locale;
  language = language.substr(0, language.find("-"));
  // Set the linebreak language.
  if (IsLanguageSupported(language.c_str())) {
    language_ = language;
  } else {
    language_ = kDefaultLanguage;
  }

  // Set the script and the layout direction.
  auto layout_info = FindLocale(locale);
  if (layout_info == nullptr) {
    // Lookup with the language.
    layout_info = FindLocale(language.c_str());
  }
  if (layout_info != nullptr) {
    SetLayoutDirection(layout_info->direction);
    SetScript(layout_info->script);
  }
  locale_ = locale;
}

void FontManager::SetScript(const char *script) {
  uint32_t s = *reinterpret_cast<const uint32_t *>(script);
  script_ = static_cast<hb_script_t>(s >> 24 | (s & 0xff0000) >> 8 |
                                     (s & 0xff00) << 8 | s << 24);
}

void FontManager::SetLanguageSettings() {
  assert(harfbuzz_buf_);
  // Set harfbuzz settings.
  if (layout_direction_ == kTextLayoutDirectionRTL) {
    hb_buffer_set_direction(harfbuzz_buf_, HB_DIRECTION_RTL);
  } else {
    hb_buffer_set_direction(harfbuzz_buf_, HB_DIRECTION_LTR);
  }
  hb_buffer_set_script(harfbuzz_buf_, static_cast<hb_script_t>(script_));
}

const GlyphCacheEntry *FontManager::GetCachedEntry(uint32_t code_point,
                                                   uint32_t ysize,
                                                   GlyphFlags flags) {
  auto glyph_info = current_font_->GetGlyphInfo(code_point);
  GlyphKey key(glyph_info->GetFaceData()->font_id_, code_point, ysize, flags);
  auto cache = glyph_cache_->Find(key);

  if (cache == nullptr) {
    // Load glyph using harfbuzz layout information.
    // Note that harfbuzz takes care of ligatures.
    FT_Error err = FT_Load_Glyph(glyph_info->GetFtFace(),
                                 glyph_info->GetCodepoint(), FT_LOAD_RENDER);
    if (err) {
      // Error. This could happen typically the loaded font does not support
      // particular glyph.
      LogInfo("Can't load glyph %c FT_Error:%d\n", code_point, err);
      return nullptr;
    }

    // Store the glyph to cache.
    FT_GlyphSlot g = glyph_info->GetFtFace()->glyph;
    GlyphCacheEntry entry;
    entry.set_code_point(code_point);
    GlyphKey new_key(glyph_info->GetFaceData()->font_id_, code_point, ysize,
                     flags);
    if (flags & (kGlyphFlagsOuterSDF | kGlyphFlagsInnerSDF) &&
        g->bitmap.width && g->bitmap.rows) {
      // Adjust a glyph size and an offset with a padding.
      entry.set_offset(vec2i(g->bitmap_left - kGlyphCachePaddingSDF,
                             g->bitmap_top + kGlyphCachePaddingSDF));
      entry.set_size(vec2i(g->bitmap.width + kGlyphCachePaddingSDF * 2,
                           g->bitmap.rows + kGlyphCachePaddingSDF * 2));
      cache = glyph_cache_->Set(nullptr, new_key, entry);
      if (cache != nullptr) {
        // Generates SDF.
        auto pos = cache->get_pos();
        auto stride = glyph_cache_->get_size().x();
        auto p = glyph_cache_->get_buffer(pos.z()) + pos.x() + pos.y() * stride;
        Grid<uint8_t> src(g->bitmap.buffer,
                          vec2i(g->bitmap.width, g->bitmap.rows),
                          kGlyphCachePaddingSDF, g->bitmap.width);
        Grid<uint8_t> dest(p, cache->get_size(), 0, stride);
        sdf_computer_.Compute(src, &dest, flags);
      }
    } else {
      entry.set_offset(vec2i(g->bitmap_left, g->bitmap_top));
      entry.set_size(vec2i(g->bitmap.width, g->bitmap.rows));
      cache = glyph_cache_->Set(g->bitmap.buffer, new_key, entry);
    }

    if (cache == nullptr) {
      // Glyph cache need to be flushed.
      // Returning nullptr here for a retry.
      LogInfo("Glyph cache is full. Need to flush and re-create.\n");
      return nullptr;
    }
  }

  // With the new glyph entry added, we would need to expand the buffers.
  ExpandAtlasTexture();
  return cache;
}

void FontManager::ExpandAtlasTexture() {
  // Expand atlas texture buffer if necessary.
  size_t slices = glyph_cache_->get_num_slices();
  if (slices > atlas_textures_.size()) {
    atlas_textures_.resize(slices);
    atlas_textures_[slices - 1]
        .reset(new Texture(nullptr, fplbase::kFormatLuminance, false));
    // Give a texture size but don't have to clear the texture here.
    atlas_textures_[slices - 1]
        ->LoadFromMemory(nullptr, glyph_cache_->get_size(), false);
    atlas_indices_.resize(slices, kInvalidSliceIndex);
  }
}

int32_t FontManager::ConvertSize(int32_t original_ysize) {
  if (size_selector_ != nullptr) {
    return size_selector_(original_ysize);
  } else {
    return original_ysize;
  }
}

void FontBuffer::AddVertices(const vec2 &pos, int32_t base_line, float scale,
                             const GlyphCacheEntry &entry) {
  mathfu::vec2i rounded_pos = mathfu::vec2i(pos);
  auto scaled_offset = mathfu::vec2(entry.get_offset()) * scale;
  auto scaled_size = mathfu::vec2(entry.get_size()) * scale;
  float scaled_base_line = base_line * scale;

  auto x = rounded_pos.x() + scaled_offset.x();
  auto y = rounded_pos.y() + scaled_base_line - scaled_offset.y();
  auto uv = entry.get_uv();
  vertices_.push_back(FontVertex(x, y, 0.0f, uv.x(), uv.y()));

  vertices_.push_back(FontVertex(x, y + scaled_size.y(), 0.0f, uv.x(), uv.w()));

  vertices_.push_back(FontVertex(x + scaled_size.x(), y, 0.0f, uv.z(), uv.y()));

  vertices_.push_back(FontVertex(x + scaled_size.x(), y + scaled_size.y(), 0.0f,
                                 uv.z(), uv.w()));
}

void FontBuffer::UpdateUV(int32_t index, const vec4 &uv) {
  vertices_[index * 4].uv_ = uv.xy();
  vertices_[index * 4 + 1].uv_ = mathfu::vec2(uv.x(), uv.w());
  vertices_[index * 4 + 2].uv_ = mathfu::vec2(uv.z(), uv.y());
  vertices_[index * 4 + 3].uv_ = uv.zw();
}

void FontBuffer::AddCaretPosition(const vec2 &pos) {
  mathfu::vec2i rounded_pos = mathfu::vec2i(pos);
  AddCaretPosition(rounded_pos.x(), rounded_pos.y());
}

void FontBuffer::AddCaretPosition(int32_t x, int32_t y) {
  assert(caret_positions_.capacity());
  caret_positions_.push_back(mathfu::vec2i(x, y));
}

void FontBuffer::AddWordBoundary(const FontBufferParameters &parameters) {
  if (parameters.get_text_alignment() & kTextAlignmentJustify) {
    // Keep the word boundary info for a later use for a justificaiton.
    word_boundary_.push_back(code_points_.size());
    word_boundary_caret_.push_back(caret_positions_.size());
  }
}

void FontBuffer::UpdateLine(const FontBufferParameters &parameters,
                            TextLayoutDirection layout_direction,
                            int32_t line_width, bool last_line) {
  // Update previous line layout if necessary.
  auto align = parameters.get_text_alignment() & ~kTextAlignmentJustify;
  auto justify = parameters.get_text_alignment() & kTextAlignmentJustify;
  if (last_line) {
    justify = false;
  }

  if (justify || align != kTextAlignmentLeft) {
    // Adjust glyph positions.
    auto offset = 0;  // Offset to add for each glyph position.
                      // When we justify a text, the offset is increased for
                      // each word boundary by boundary_offset_change.
    auto boundary_offset_change = 0;
    if (justify && word_boundary_.size() > 1) {
      // With a justification, we add an offset for each word boundary.
      // For each word boundary (e.g. spaces), we stretch them slightly to align
      // both the left and right ends of each line of text.
      boundary_offset_change = (parameters.get_size().x() - line_width) /
                               (word_boundary_.size() - 1);
    } else {
      justify = false;
      offset = parameters.get_size().x() - line_width;
      if (align == kTextAlignmentCenter) {
        offset = offset / 2;  // Centering.
      }
    }

    // Flip the value if the text layout is in RTL.
    if (layout_direction == kTextLayoutDirectionRTL) {
      offset = -offset;
      boundary_offset_change = -boundary_offset_change;
    }
    // Keep original offset value.
    auto offset_caret = offset;

    // Update each glyph's position.
    auto boundary_index = 0;
    for (auto idx = line_start_index_; idx < code_points_.size(); ++idx) {
      if (justify && idx >= word_boundary_[boundary_index]) {
        boundary_index++;
        offset += boundary_offset_change;
      }
      auto it = vertices_.begin() + idx * kVerticesPerCodePoint;
      for (auto i = 0; i < kVerticesPerCodePoint; ++i) {
        it->position_.data[0] += offset;
        it++;
      }
    }

    // Update caret position, too.
    if (HasCaretPositions()) {
      boundary_index = 0;
      for (auto idx = line_start_caret_index_; idx < caret_positions_.size();
           ++idx) {
        if (justify && idx >= word_boundary_caret_[boundary_index]) {
          boundary_index++;
          offset_caret += boundary_offset_change;
        }
        caret_positions_[idx].x() += offset_caret;
      }
    }
  }

  // Update current line information.
  line_start_index_ = code_points_.size();
  line_start_caret_index_ = caret_positions_.size();
  word_boundary_.clear();
  word_boundary_caret_.clear();
}

void FaceData::Close() {
  // Remove the font data associated to this face data.
  HbFont::Close(*this);
  FT_Done_Face(face_);
  font_data_.clear();
}

void GlyphCacheRow::InvalidateReferencingBuffers() {
  auto begin = ref_.begin();
  auto end = ref_.end();
  while (begin != end) {
    auto buffer = *begin;
    buffer->Invalidate();
    begin++;
  }
}

}  // namespace flatui
