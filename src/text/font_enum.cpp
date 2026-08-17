/*
Slugged - GPU vector text for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "font_enum.hpp"

#include <algorithm>
#include <cstdint>

// Platform font discovery. Each backend answers three questions -- what is
// installed, which file best matches a family/weight/slant, and what covers a
// codepoint the chosen font lacks -- and nothing else. Shaping and outlining
// always go through HarfBuzz and FreeType, so the same document renders
// identically on all three platforms.

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#else
#include <fontconfig/fontconfig.h>
#endif

namespace slugged {
namespace fontenum {

// ---------------------------------------------------------------------------
#if defined(_WIN32)
// ---------------------------------------------------------------------------

using Microsoft::WRL::ComPtr;

namespace {

IDWriteFactory *factory()
{
	static IDWriteFactory *f = nullptr;
	static bool tried = false;

	if (!tried) {
		tried = true;

		if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
					       reinterpret_cast<IUnknown **>(&f))))
			f = nullptr;
	}

	return f;
}

std::string toUtf8(const wchar_t *w)
{
	if (!w)
		return {};

	const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);

	if (len <= 1)
		return {};

	std::string out(size_t(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);

	return out;
}

// Pulls the on-disk path out of a resolved DirectWrite font face. DirectWrite
// can describe fonts that have no file at all (memory-loaded collections); those
// are skipped rather than guessed at.
bool fileOf(IDWriteFont *font, FontFileRef &out)
{
	ComPtr<IDWriteFontFace> face;

	if (!font || FAILED(font->CreateFontFace(&face)) || !face)
		return false;

	UINT32 numFiles = 0;

	if (FAILED(face->GetFiles(&numFiles, nullptr)) || numFiles == 0)
		return false;

	std::vector<IDWriteFontFile *> files(numFiles, nullptr);

	if (FAILED(face->GetFiles(&numFiles, files.data())))
		return false;

	bool ok = false;

	if (files[0]) {
		const void *key = nullptr;
		UINT32 keySize = 0;
		ComPtr<IDWriteFontFileLoader> loader;

		if (SUCCEEDED(files[0]->GetReferenceKey(&key, &keySize)) && SUCCEEDED(files[0]->GetLoader(&loader))) {
			ComPtr<IDWriteLocalFontFileLoader> local;

			if (SUCCEEDED(loader.As(&local))) {
				UINT32 pathLen = 0;

				if (SUCCEEDED(local->GetFilePathLengthFromKey(key, keySize, &pathLen))) {
					std::wstring path(pathLen + 1, L'\0');

					if (SUCCEEDED(local->GetFilePathFromKey(key, keySize, path.data(),
										pathLen + 1))) {
						out.path = toUtf8(path.c_str());
						out.faceIndex = int(face->GetIndex());
						ok = !out.path.empty();
					}
				}
			}
		}
	}

	for (IDWriteFontFile *f : files)
		if (f)
			f->Release();

	return ok;
}

} // namespace

std::vector<std::string> families()
{
	std::vector<std::string> out;

	IDWriteFactory *f = factory();

	if (!f)
		return out;

	ComPtr<IDWriteFontCollection> collection;

	if (FAILED(f->GetSystemFontCollection(&collection, FALSE)) || !collection)
		return out;

	const UINT32 count = collection->GetFontFamilyCount();

	for (UINT32 i = 0; i < count; i++) {
		ComPtr<IDWriteFontFamily> family;

		if (FAILED(collection->GetFontFamily(i, &family)) || !family)
			continue;

		ComPtr<IDWriteLocalizedStrings> names;

		if (FAILED(family->GetFamilyNames(&names)) || !names)
			continue;

		UINT32 index = 0;
		BOOL exists = FALSE;

		if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists)
			index = 0;

		UINT32 len = 0;

		if (FAILED(names->GetStringLength(index, &len)))
			continue;

		std::wstring name(len + 1, L'\0');

		if (SUCCEEDED(names->GetString(index, name.data(), len + 1)))
			out.push_back(toUtf8(name.c_str()));
	}

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());

	return out;
}

bool match(const std::string &family, int weight, bool italic, FontFileRef &out)
{
	IDWriteFactory *f = factory();

	if (!f || family.empty())
		return false;

	ComPtr<IDWriteFontCollection> collection;

	if (FAILED(f->GetSystemFontCollection(&collection, FALSE)) || !collection)
		return false;

	const int wlen = MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, nullptr, 0);
	std::wstring wide(size_t(wlen), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, wide.data(), wlen);

	UINT32 index = 0;
	BOOL exists = FALSE;

	if (FAILED(collection->FindFamilyName(wide.c_str(), &index, &exists)) || !exists)
		return false;

	ComPtr<IDWriteFontFamily> fam;

	if (FAILED(collection->GetFontFamily(index, &fam)) || !fam)
		return false;

	ComPtr<IDWriteFont> font;

	if (FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT(weight), DWRITE_FONT_STRETCH_NORMAL,
					     italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font)) ||
	    !font)
		return false;

	return fileOf(font.Get(), out);
}

std::vector<FontFileRef> fallbacks(uint32_t codepoint, const std::string &preferredFamily)
{
	std::vector<FontFileRef> out;

	IDWriteFactory *f = factory();

	if (!f)
		return out;

	// DirectWrite's IDWriteFontFallback::MapCharacters would answer this more
	// cheaply, but it requires a full IDWriteTextAnalysisSource implementation
	// for what is a single-codepoint question. Scanning the system collection
	// for coverage gets the same answer with far less COM surface; FontManager
	// caches the result per codepoint, so the scan happens once per missing
	// character rather than once per layout.
	ComPtr<IDWriteFontCollection> collection;

	if (FAILED(f->GetSystemFontCollection(&collection, FALSE)) || !collection)
		return out;

	const UINT32 count = collection->GetFontFamilyCount();

	for (UINT32 i = 0; i < count && out.size() < 8; i++) {
		ComPtr<IDWriteFontFamily> family;

		if (FAILED(collection->GetFontFamily(i, &family)) || !family)
			continue;

		ComPtr<IDWriteFont> font;

		if (FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
							DWRITE_FONT_STYLE_NORMAL, &font)) ||
		    !font)
			continue;

		BOOL covers = FALSE;

		if (FAILED(font->HasCharacter(codepoint, &covers)) || !covers)
			continue;

		FontFileRef ref;

		if (fileOf(font.Get(), ref))
			out.push_back(std::move(ref));
	}

	(void)preferredFamily;

	return out;
}

std::string defaultFamily()
{
	return "Segoe UI";
}

// ---------------------------------------------------------------------------
#elif defined(__APPLE__)
// ---------------------------------------------------------------------------

namespace {

std::string toUtf8(CFStringRef s)
{
	if (!s)
		return {};

	const CFIndex len = CFStringGetLength(s);
	const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;

	std::string out(size_t(max), '\0');

	if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8))
		return {};

	out.resize(std::char_traits<char>::length(out.c_str()));

	return out;
}

bool fileOf(CTFontDescriptorRef desc, FontFileRef &out)
{
	if (!desc)
		return false;

	CFURLRef url = static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute));

	if (!url)
		return false;

	char path[2048] = {0};
	const bool ok = CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8 *>(path), sizeof(path));

	CFRelease(url);

	if (!ok)
		return false;

	out.path = path;
	out.faceIndex = 0;

	return true;
}

CFStringRef makeCFString(const std::string &s)
{
	return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

} // namespace

std::vector<std::string> families()
{
	std::vector<std::string> out;

	CFArrayRef names = CTFontManagerCopyAvailableFontFamilyNames();

	if (!names)
		return out;

	const CFIndex count = CFArrayGetCount(names);

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef name = static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, i));
		std::string s = toUtf8(name);

		if (!s.empty())
			out.push_back(std::move(s));
	}

	CFRelease(names);

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());

	return out;
}

bool match(const std::string &family, int weight, bool italic, FontFileRef &out)
{
	if (family.empty())
		return false;

	CFStringRef name = makeCFString(family);

	if (!name)
		return false;

	// CoreText's weight axis is -1..1 rather than 100..900; 400 maps to 0.
	const float ctWeight = (float(weight) - 400.0f) / 500.0f;

	CFMutableDictionaryRef traits = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef weightNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &ctWeight);
	CFDictionarySetValue(traits, kCTFontWeightTrait, weightNum);

	if (italic) {
		const uint32_t symbolic = kCTFontTraitItalic;
		CFNumberRef sym = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &symbolic);
		CFDictionarySetValue(traits, kCTFontSymbolicTrait, sym);
		CFRelease(sym);
	}

	CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
								 &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(attrs, kCTFontFamilyNameAttribute, name);
	CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traits);

	CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);

	bool ok = false;

	if (desc) {
		CTFontRef font = CTFontCreateWithFontDescriptor(desc, 12.0, nullptr);

		if (font) {
			CTFontDescriptorRef resolved = CTFontCopyFontDescriptor(font);

			ok = fileOf(resolved, out);

			if (resolved)
				CFRelease(resolved);

			CFRelease(font);
		}

		CFRelease(desc);
	}

	CFRelease(attrs);
	CFRelease(weightNum);
	CFRelease(traits);
	CFRelease(name);

	return ok;
}

std::vector<FontFileRef> fallbacks(uint32_t codepoint, const std::string &preferredFamily)
{
	std::vector<FontFileRef> out;

	CFStringRef base = makeCFString(preferredFamily.empty() ? defaultFamily() : preferredFamily);

	if (!base)
		return out;

	CTFontRef font = CTFontCreateWithName(base, 12.0, nullptr);
	CFRelease(base);

	if (!font)
		return out;

	// UTF-16 encode the codepoint and ask CoreText which font it would use.
	UniChar buf[2];
	CFIndex len = 1;

	if (codepoint > 0xFFFF) {
		const uint32_t v = codepoint - 0x10000;
		buf[0] = UniChar(0xD800 + (v >> 10));
		buf[1] = UniChar(0xDC00 + (v & 0x3FF));
		len = 2;
	} else {
		buf[0] = UniChar(codepoint);
	}

	CFStringRef str = CFStringCreateWithCharacters(kCFAllocatorDefault, buf, len);

	if (str) {
		CTFontRef sub = CTFontCreateForString(font, str, CFRangeMake(0, len));

		if (sub) {
			CTFontDescriptorRef desc = CTFontCopyFontDescriptor(sub);
			FontFileRef ref;

			if (fileOf(desc, ref))
				out.push_back(std::move(ref));

			if (desc)
				CFRelease(desc);

			CFRelease(sub);
		}

		CFRelease(str);
	}

	CFRelease(font);

	return out;
}

std::string defaultFamily()
{
	return "Helvetica Neue";
}

// ---------------------------------------------------------------------------
#else // fontconfig
// ---------------------------------------------------------------------------

namespace {

bool initFontconfig()
{
	static const bool ok = FcInit() == FcTrue;

	return ok;
}

int toFcWeight(int cssWeight)
{
	// fontconfig's own scale, not CSS's.
	if (cssWeight <= 100)
		return FC_WEIGHT_THIN;
	if (cssWeight <= 200)
		return FC_WEIGHT_EXTRALIGHT;
	if (cssWeight <= 300)
		return FC_WEIGHT_LIGHT;
	if (cssWeight <= 400)
		return FC_WEIGHT_REGULAR;
	if (cssWeight <= 500)
		return FC_WEIGHT_MEDIUM;
	if (cssWeight <= 600)
		return FC_WEIGHT_DEMIBOLD;
	if (cssWeight <= 700)
		return FC_WEIGHT_BOLD;
	if (cssWeight <= 800)
		return FC_WEIGHT_EXTRABOLD;

	return FC_WEIGHT_BLACK;
}

bool refFromPattern(FcPattern *pattern, FontFileRef &out)
{
	FcChar8 *file = nullptr;

	if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch || !file)
		return false;

	int index = 0;
	FcPatternGetInteger(pattern, FC_INDEX, 0, &index);

	out.path = reinterpret_cast<const char *>(file);
	out.faceIndex = index;

	return true;
}

} // namespace

std::vector<std::string> families()
{
	std::vector<std::string> out;

	if (!initFontconfig())
		return out;

	FcPattern *pattern = FcPatternCreate();
	FcObjectSet *objects = FcObjectSetBuild(FC_FAMILY, nullptr);
	FcFontSet *set = FcFontList(nullptr, pattern, objects);

	if (set) {
		for (int i = 0; i < set->nfont; i++) {
			FcChar8 *family = nullptr;

			if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &family) == FcResultMatch && family)
				out.push_back(reinterpret_cast<const char *>(family));
		}

		FcFontSetDestroy(set);
	}

	if (objects)
		FcObjectSetDestroy(objects);

	if (pattern)
		FcPatternDestroy(pattern);

	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());

	return out;
}

bool match(const std::string &family, int weight, bool italic, FontFileRef &out)
{
	if (!initFontconfig() || family.empty())
		return false;

	FcPattern *pattern = FcPatternCreate();

	if (!pattern)
		return false;

	FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(family.c_str()));
	FcPatternAddInteger(pattern, FC_WEIGHT, toFcWeight(weight));
	FcPatternAddInteger(pattern, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);

	FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	FcResult result = FcResultNoMatch;
	FcPattern *matched = FcFontMatch(nullptr, pattern, &result);

	bool ok = false;

	if (matched && result == FcResultMatch) {
		// fontconfig always returns *something*; verify it actually gave us
		// the family that was asked for, otherwise the caller's own fallback
		// logic should get a chance first.
		FcChar8 *gotFamily = nullptr;

		if (FcPatternGetString(matched, FC_FAMILY, 0, &gotFamily) == FcResultMatch && gotFamily) {
			const std::string got = reinterpret_cast<const char *>(gotFamily);

			auto lower = [](std::string s) {
				std::transform(s.begin(), s.end(), s.begin(),
					       [](unsigned char c) { return char(::tolower(c)); });
				return s;
			};

			if (lower(got) == lower(family))
				ok = refFromPattern(matched, out);
		}
	}

	if (matched)
		FcPatternDestroy(matched);

	FcPatternDestroy(pattern);

	return ok;
}

std::vector<FontFileRef> fallbacks(uint32_t codepoint, const std::string &preferredFamily)
{
	std::vector<FontFileRef> out;

	if (!initFontconfig())
		return out;

	FcPattern *pattern = FcPatternCreate();

	if (!pattern)
		return out;

	if (!preferredFamily.empty())
		FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(preferredFamily.c_str()));

	// Ask fontconfig for fonts covering exactly this codepoint. This is the
	// whole reason to use it rather than a hardcoded list: it knows what is
	// installed and which of those cover the character.
	FcCharSet *charset = FcCharSetCreate();
	FcCharSetAddChar(charset, FcChar32(codepoint));
	FcPatternAddCharSet(pattern, FC_CHARSET, charset);

	FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	FcResult result = FcResultNoMatch;
	FcFontSet *set = FcFontSort(nullptr, pattern, FcTrue, nullptr, &result);

	if (set) {
		for (int i = 0; i < set->nfont && out.size() < 8; i++) {
			FcCharSet *cs = nullptr;

			if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch && cs &&
			    FcCharSetHasChar(cs, FcChar32(codepoint))) {
				FontFileRef ref;

				if (refFromPattern(set->fonts[i], ref))
					out.push_back(std::move(ref));
			}
		}

		FcFontSetSortDestroy(set);
	}

	FcCharSetDestroy(charset);
	FcPatternDestroy(pattern);

	return out;
}

std::string defaultFamily()
{
	return "DejaVu Sans";
}

#endif

} // namespace fontenum
} // namespace slugged
