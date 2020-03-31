// Copyright © 2008-2020 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "PiGui.h"
#include "Input.h"
#include "Pi.h"

#include "graphics/Texture.h"
#include "graphics/opengl/RendererGL.h"
#include "graphics/opengl/TextureGL.h" // nasty, usage of GL is implementation specific
#include "imgui/imgui.h"

// Use GLEW instead of GL3W.
#define IMGUI_IMPL_OPENGL_LOADER_GLEW 1
#include "imgui/examples/imgui_impl_opengl3.h"
#include "imgui/examples/imgui_impl_sdl.h"

#include <float.h>
#include <stdio.h>
#include <string.h>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

using namespace PiGui;

std::vector<Graphics::Texture *> m_svg_textures;

std::vector<Graphics::Texture *> &PiGui::GetSVGTextures()
{
	return m_svg_textures;
}

static void *makeTexture(unsigned char *pixels, int width, int height)
{
	PROFILE_SCOPED()
	// this is not very pretty code and uses the Graphics::TextureGL class directly
	// Texture descriptor defines the size, type.
	// Gone for LINEAR_CLAMP here and RGBA like the original code
	const vector2f texSize(1.0f, 1.0f);
	const vector3f dataSize(width, height, 0.0f);
	const Graphics::TextureDescriptor texDesc(Graphics::TEXTURE_RGBA_8888,
		dataSize, texSize, Graphics::LINEAR_CLAMP,
		false, false, false, 0, Graphics::TEXTURE_2D);
	// Create the texture, calling it via renderer directly avoids the caching call of TextureBuilder
	// However interestingly this gets called twice which would have been a WIN for the TextureBuilder :/
	Graphics::Texture *pTex = Pi::renderer->CreateTexture(texDesc);
	// Update it with the actual pixels, this is a two step process due to legacy code
	pTex->Update(pixels, dataSize, Graphics::TEXTURE_RGBA_8888);
	// nasty bit as I invoke the TextureGL
	Graphics::OGL::TextureGL *pGLTex = reinterpret_cast<Graphics::OGL::TextureGL *>(pTex);
	Uint32 result = pGLTex->GetTextureID();
	PiGui::GetSVGTextures().push_back(pTex); // store for cleanup later
	return reinterpret_cast<void *>(result);
}

ImTextureID PiGui::RenderSVG(std::string svgFilename, int width, int height)
{
	PROFILE_SCOPED()
	Output("nanosvg: %s %dx%d\n", svgFilename.c_str(), width, height);

	// // re-use existing texture if already loaded
	// for(auto strTex : m_svg_textures) {
	// 	if(strTex.first == svgFilename) {
	// 		// nasty bit as I invoke the TextureGL
	// 		Graphics::TextureGL *pGLTex = reinterpret_cast<Graphics::TextureGL*>(strTex.second);
	// 		Uint32 result = pGLTex->GetTexture();
	// 		Output("Re-used existing texture with id: %i\n", result);
	// 		return reinterpret_cast<void*>(result);
	// 	}
	// }

	NSVGimage *image = NULL;
	NSVGrasterizer *rast = NULL;
	unsigned char *img = NULL;
	int w;
	// size of each icon
	//	int size = 64;
	// 16 columns
	//	int W = 16*size;
	int W = width;
	// 16 rows
	//	int H = 16*size;
	int H = height;
	img = static_cast<unsigned char *>(malloc(W * H * 4));
	memset(img, 0, W * H * 4);
	{
		PROFILE_SCOPED_DESC("nsvgParseFromFile")
		image = nsvgParseFromFile(svgFilename.c_str(), "px", 96.0f);
		if (image == NULL) {
			Error("Could not open SVG image.\n");
		}
	}
	w = static_cast<int>(image->width);

	rast = nsvgCreateRasterizer();
	if (rast == NULL) {
		Error("Could not init rasterizer.\n");
	}

	if (img == NULL) {
		Error("Could not alloc image buffer.\n");
	}
	{
		PROFILE_SCOPED_DESC("nsvgRasterize")
		float scale = double(W) / w;
		float tx = 0;
		float ty = 0;
		nsvgRasterize(rast, image, tx, ty, scale, img, W, H, W * 4);
	}
	nsvgDeleteRasterizer(rast);
	nsvgDelete(image);
	return makeTexture(img, W, H);
}

//
//	PiGui::Instance
//

Instance::Instance() :
	m_should_bake_fonts(true)
{
	// TODO: clang-format doesn't like list initializers inside function calls
	// clang-format off
	PiFont uiheading("orbiteer", {
		PiFace("DejaVuSans.ttf", /*18.0/20.0*/ 1.2),
		PiFace("wqy-microhei.ttc", 1.0),
		PiFace("Orbiteer-Bold.ttf", 1.0) // imgui only supports 0xffff, not 0x10ffff
	});
	AddFontDefinition(uiheading);

	PiFont guifont("pionillium", {
		PiFace("DejaVuSans.ttf", 13.0 / 14.0),
		PiFace("wqy-microhei.ttc", 1.0),
		PiFace("PionilliumText22L-Medium.ttf", 1.0)
	});
	AddFontDefinition(guifont);
	// clang-format on

	// Output("Fonts:\n");
	for (auto entry : m_font_definitions) {
		//		Output("  entry %s:\n", entry.first.c_str());
		entry.second.describe();
	}

	// ensure the tooltip font exists
	GetFont("pionillium", 14);
};

ImFont *Instance::GetFont(const std::string &name, int size)
{
	PROFILE_SCOPED()
	auto iter = m_fonts.find(std::make_pair(name, size));
	if (iter != m_fonts.end())
		return iter->second;
	//	Output("GetFont: adding font %s at %i on demand\n", name.c_str(), size);
	ImFont *font = AddFont(name, size);

	return font;
}

void Instance::AddGlyph(ImFont *font, unsigned short glyph)
{
	PROFILE_SCOPED()
	// range glyph..glyph
	auto iter = m_im_fonts.find(font);
	if (iter == m_im_fonts.end()) {
		Error("Cannot find font instance for ImFont %p\n", font);
		assert(false);
	}
	auto pifont_iter = m_pi_fonts.find(iter->second);
	if (pifont_iter == m_pi_fonts.end()) {
		Error("No registered PiFont for name %s size %i\n", iter->second.first.c_str(), iter->second.second);
		assert(false);
	}
	PiFont &pifont = pifont_iter->second;
	for (PiFace &face : pifont.faces()) {
		if (face.isValidGlyph(glyph)) {
			face.addGlyph(glyph);
			m_should_bake_fonts = true;
			return;
		}
	}
	Error("No face in font %s handles glyph %i\n", pifont.name().c_str(), glyph);
}

ImFont *Instance::AddFont(const std::string &name, int size)
{
	PROFILE_SCOPED()
	auto iter = m_font_definitions.find(name);
	if (iter == m_font_definitions.end()) {
		Error("No font definition with name %s\n", name.c_str());
		assert(false);
	}
	auto existing = m_fonts.find(std::make_pair(name, size));
	if (existing != m_fonts.end()) {
		Error("Font %s already exists at size %i\n", name.c_str(), size);
		assert(false);
	}

	PiFont &pifont = iter->second;
	pifont.setPixelsize(size);
	pifont.faces().back().addGlyph(0x20); // only add space
	m_pi_fonts[std::make_pair(name, size)] = pifont;

	m_should_bake_fonts = true;

	return m_fonts[std::make_pair(name, size)];
}

void Instance::RefreshFontsTexture()
{
	PROFILE_SCOPED()
	// TODO: fix this, do the right thing, don't just re-create *everything* :)
	ImGui::GetIO().Fonts->Build();
	ImGui_ImplOpenGL3_CreateDeviceObjects();
}

void PiDefaultStyle(ImGuiStyle &style)
{
	PROFILE_SCOPED()
	style.WindowBorderSize = 0.0f; // Thickness of border around windows. Generally set to 0.0f or 1.0f. Other values not well tested.
}

void Instance::Init(SDL_Window *window)
{
	PROFILE_SCOPED()

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	// TODO: FIXME before upgrading! The sdl_gl_context parameter is currently
	// unused, but that is slated to change very soon.
	// We will need to fill this with a valid pointer to the OpenGL context.
	ImGui_ImplSDL2_InitForOpenGL(window, NULL);
	switch (Pi::renderer->GetRendererType()) {
	default:
	case Graphics::RENDERER_DUMMY:
		Error("RENDERER_DUMMY is not a valid renderer, aborting.");
		return;
	case Graphics::RENDERER_OPENGL_3x:
		ImGui_ImplOpenGL3_Init();
		break;
	}

	ImGuiIO &io = ImGui::GetIO();
	// Apply the base style
	ImGui::StyleColorsDark();

	// Apply Pioneer's style.
	// TODO: load this from Lua.
	PiDefaultStyle(ImGui::GetStyle());

	std::string imguiIni = FileSystem::JoinPath(FileSystem::GetUserDir(), "imgui.ini");
	// this will be leaked, not sure how to deal with it properly in imgui...
	char *ioIniFilename = new char[imguiIni.size() + 1];
	std::strncpy(ioIniFilename, imguiIni.c_str(), imguiIni.size() + 1);
	io.IniFilename = ioIniFilename;
}

bool Instance::ProcessEvent(SDL_Event *event)
{
	PROFILE_SCOPED()
	ImGui_ImplSDL2_ProcessEvent(event);
	return false;
}

void Instance::NewFrame(SDL_Window *window)
{
	PROFILE_SCOPED()

	// Ask ImGui to hide OS cursor if we're capturing it for input:
	// it will do this if GetMouseCursor == ImGuiMouseCursor_None.
	if (Pi::input->IsCapturingMouse()) {
		ImGui::SetMouseCursor(ImGuiMouseCursor_None);
	}

	switch (Pi::renderer->GetRendererType()) {
	default:
	case Graphics::RENDERER_DUMMY:
		Error("RENDERER_DUMMY is not a valid renderer, aborting.");
		return;
	case Graphics::RENDERER_OPENGL_3x:
		ImGui_ImplOpenGL3_NewFrame();
		break;
	}
	ImGui_ImplSDL2_NewFrame(window);
	ImGui::NewFrame();

	Pi::renderer->CheckRenderErrors(__FUNCTION__, __LINE__);
	ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
}

void Instance::EndFrame()
{
	PROFILE_SCOPED()

	// Explicitly end frame, to show tooltips. Otherwise, they are shown at the next NextFrame,
	// which might crash because the font atlas was rebuilt, and the old fonts were cached inside imgui.
	ImGui::EndFrame();

	// Iterate through our fonts and check if IMGUI wants a character we don't have.
	for (auto &iter : m_fonts) {
		ImFont *font = iter.second;
		// font might be nullptr, if it wasn't baked yet
		if (font && !font->MissingGlyphs.empty()) {
			//			Output("%s %i is missing glyphs.\n", iter.first.first.c_str(), iter.first.second);
			for (const auto &glyph : font->MissingGlyphs) {
				AddGlyph(font, glyph);
			}
			font->MissingGlyphs.clear();
		}
	}

	// Bake fonts *after* a frame is done, so the font atlas is not needed any longer
	if (m_should_bake_fonts) {
		BakeFonts();
	}
}

void Instance::Render()
{
	PROFILE_SCOPED()
	EndFrame();

	ImGui::Render();

	switch (Pi::renderer->GetRendererType()) {
	default:
	case Graphics::RENDERER_DUMMY:
		return;
	case Graphics::RENDERER_OPENGL_3x:
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		break;
	}
}

void Instance::ClearFonts()
{
	PROFILE_SCOPED()
	ImGuiIO &io = ImGui::GetIO();
	// TODO: should also release all glyph_ranges...
	m_fonts.clear();
	m_im_fonts.clear();
	io.Fonts->Clear();
}

void Instance::BakeFont(PiFont &font)
{
	PROFILE_SCOPED()
	ImGuiIO &io = ImGui::GetIO();
	ImFont *imfont = nullptr;
	for (PiFace &face : font.faces()) {
		ImFontConfig config;
		config.MergeMode = true;
		float size = font.pixelsize() * face.sizefactor();
		const std::string path = FileSystem::JoinPath(FileSystem::JoinPath(FileSystem::GetDataDir(), "fonts"), face.ttfname());
		//		Output("- baking face %s at size %f\n", path.c_str(), size);
		face.sortUsedRanges();
		if (face.used_ranges().size() > 0) {
			face.m_imgui_ranges.clear();
			ImFontAtlas::GlyphRangesBuilder gb;
			// Always include the default range
			gb.AddRanges(io.Fonts->GetGlyphRangesDefault());
			ImWchar gr[3] = { 0, 0, 0 };
			for (auto &range : face.used_ranges()) {
				// Output("Used range: %x - %x", range.first, range.second);
				gr[0] = range.first;
				gr[1] = range.second;
				gb.AddRanges(gr);
			}
			gb.BuildRanges(&face.m_imgui_ranges);
			ImFont *f = io.Fonts->AddFontFromFileTTF(path.c_str(), size, imfont == nullptr ? nullptr : &config, face.m_imgui_ranges.Data);
			assert(f);
			if (imfont != nullptr)
				assert(f == imfont);
			imfont = f;
		}
	}
	m_im_fonts[imfont] = std::make_pair(font.name(), font.pixelsize());
	// 	Output("setting %s %i to %p\n", font.name(), font.pixelsize(), imfont);
	m_fonts[std::make_pair(font.name(), font.pixelsize())] = imfont;
	if (!imfont->MissingGlyphs.empty()) {
		Output("WARNING: glyphs missing in shiny new font\n");
	}
	imfont->MissingGlyphs.clear();
}

void Instance::BakeFonts()
{
	PROFILE_SCOPED()
	//	Output("Baking fonts\n");

	m_should_bake_fonts = false;

	if (m_pi_fonts.size() == 0) {
		//		Output("No fonts to bake.\n");
		return;
	}

	ClearFonts();

	// first bake tooltip/default font
	BakeFont(m_pi_fonts[std::make_pair("pionillium", 14)]);

	for (auto &iter : m_pi_fonts) {
		// don't bake tooltip/default font again
		if (!(iter.first.first == "pionillium" && iter.first.second == 14))
			BakeFont(iter.second);
		//		Output("Fonts registered: %i\n", io.Fonts->Fonts.Size);
	}

	RefreshFontsTexture();
}

void Instance::Uninit()
{
	PROFILE_SCOPED()
	for (auto tex : m_svg_textures) {
		delete tex;
	}

	switch (Pi::renderer->GetRendererType()) {
	default:
	case Graphics::RENDERER_DUMMY:
		return;
	case Graphics::RENDERER_OPENGL_3x:
		ImGui_ImplOpenGL3_Shutdown();
		break;
	}

	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
}

//
// PiGui::PiFace
//

const bool PiFace::isValidGlyph(unsigned short glyph) const
{
	PROFILE_SCOPED()
	return (m_invalid_glyphs.count(glyph) == 0);
}

void PiFace::addGlyph(unsigned short glyph)
{
	PROFILE_SCOPED()
	// Output("- PiFace %s adding glyph 0x%x\n", ttfname().c_str(), glyph);
	for (auto &range : m_used_ranges) {
		if (range.first <= glyph && glyph <= range.second) {
			// Output(" - already added, not adding again\n");
			m_invalid_glyphs.insert(glyph); //if already added it once and trying to add it again, it's invalid
			return;
		}
	}
	//	Output(" - added\n");
	m_used_ranges.push_back(std::make_pair(glyph, glyph));
}

void PiFace::sortUsedRanges() const
{
	PROFILE_SCOPED()
	// sort by ascending lower end of range
	std::sort(m_used_ranges.begin(), m_used_ranges.end(), [](const std::pair<unsigned short, unsigned short> &a, const std::pair<unsigned short, unsigned short> &b) { return a.first < b.first; });
	// merge adjacent ranges
	std::vector<std::pair<unsigned short, unsigned short>> merged;
	std::pair<unsigned short, unsigned short> current(0xffff, 0xffff);
	for (auto &range : m_used_ranges) {
		//		Output("> checking 0x%x-0x%x\n", range.first, range.second);
		if (current.first == 0xffff && current.second == 0xffff)
			current = range;
		else {
			// if only a few are missing in range, just merge nontheless. +5 is 4 missing
			if (current.second + 5 >= range.first) { // (current.second + 1 == range.first)
				//				Output("> merging 0x%x-0x%x and 0x%x-0x%x\n", current.first, current.second, range.first, range.second);
				current.second = range.second;
			} else {
				//				Output("> pushing 0x%x-0x%x\n", current.first, current.second);
				merged.push_back(current);
				current = range;
			}
		}
	}
	if (current.first != 0xffff && current.second != 0xffff)
		merged.push_back(current);
	m_used_ranges.assign(merged.begin(), merged.end());
}
