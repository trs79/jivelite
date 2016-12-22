/*
** Copyright 2010 Logitech. All Rights Reserved.
**
** This file is licensed under BSD. Please see the LICENSE file for details.
*/


#include "common.h"
#include "jive.h"

static const char *JIVE_FONT_MAGIC = "Font";

static JiveFont *fonts = NULL;

static texture_atlas_t *atlas = NULL;

static char *font_cache =
" !\"#$%&'()*+,-./0123456789:;<=>?"
"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
"`abcdefghijklmnopqrstuvwxyz{|}~";

GPU_Image *atlas_image = NULL;

// todo, fix extern
extern GPU_Target *main_screen;

Uint32 shader_program_number;

#define GPU_TEXT_FRAGMENT_SHADER_SOURCE_GLES \
"#version 100\n\
#ifdef GL_FRAGMENT_PRECISION_HIGH\n\
precision highp float;\n\
#else\n\
precision mediump float;\n\
#endif\n\
precision mediump int;\n\
\
varying mediump vec4 color;\n\
varying vec2 texCoord;\n\
\
uniform sampler2D tex;\n\
\
void main(void)\n\
{\n\
	float a = texture2D(tex, texCoord).a; \n\
	gl_FragColor = vec4(color.rgb, color.a*a); \n\
}"

#define GPU_TEXT_VERTEX_SHADER_SOURCE_GLES \
"#version 100\n\
precision highp float;\n\
precision mediump int;\n\
\
attribute vec2 gpu_Vertex;\n\
attribute mediump vec4 gpu_Color;\n\
uniform mat4 gpu_ModelViewProjectionMatrix;\n\
\
varying mediump vec4 color;\n\
\
void main(void)\n\
{\n\
	color = gpu_Color;\n\
	gl_Position = gpu_ModelViewProjectionMatrix * vec4(gpu_Vertex, 0.0, 1.0);\n\
}"



#define GPU_TEXT_FRAGMENT_SHADER_SOURCE \
"#version 150\n\
\
in vec4 color;\n\
in vec2 texCoord;\n\
\
uniform sampler2D tex;\n\
\
out vec4 fragColor;\n\
\
void main(void)\n\
{\n\
	float a = texture(tex, texCoord).a; \n\
	fragColor = vec4(color.rgb, color.a*a); \n\
}"

#define GPU_TEXT_VERTEX_SHADER_SOURCE \
"#version 150\n\
\
in vec2 gpu_Vertex;\n\
in vec2 gpu_TexCoord;\n\
in vec4 gpu_Color;\n\
uniform mat4 gpu_ModelViewProjectionMatrix;\n\
\
out vec4 color;\n\
out vec2 texCoord;\n\
\
void main(void)\n\
{\n\
	color = gpu_Color;\n\
	texCoord = vec2(gpu_TexCoord);\n\
	gl_Position = gpu_ModelViewProjectionMatrix * vec4(gpu_Vertex, 0.0, 1.0);\n\
}"



static int load_ttf_font(JiveFont *font, const char *name, Uint16 size);

static void destroy_ttf_font(JiveFont *font);

static int width_ttf_font(JiveFont *font, const char *str);

static JiveDrawText *draw_ttf_font(JiveFont *font, Uint32 color, const char *str);



JiveFont *jive_font_load(const char *name, Uint16 size) {

	// Do we already have this font loaded?
	JiveFont *ptr = fonts;
	while (ptr) {
		if (ptr->size == size &&
		    strcmp(ptr->name, name) == 0) {
			ptr->refcount++;
			return ptr;
		}

		ptr = ptr->next;
	}


	/* Initialise the TTF api when required */
	if (!TTF_WasInit()) {
		if (TTF_Init() == -1) {
			LOG_WARN(log_ui_draw, "TTF_Init: %s\n", TTF_GetError());
			exit(-1);
		}

		int v, p;

		GPU_RendererID id = GPU_GetRendererID(GPU_RENDERER_GLES_2);

		if (id.renderer == GPU_RENDERER_GLES_2) {
			v = GPU_CompileShader(GPU_VERTEX_SHADER, GPU_TEXT_VERTEX_SHADER_SOURCE_GLES);
			p = GPU_CompileShader(GPU_FRAGMENT_SHADER, GPU_TEXT_FRAGMENT_SHADER_SOURCE_GLES);
		}
		else {
			v = GPU_CompileShader(GPU_VERTEX_SHADER, GPU_TEXT_VERTEX_SHADER_SOURCE);
			p = GPU_CompileShader(GPU_FRAGMENT_SHADER, GPU_TEXT_FRAGMENT_SHADER_SOURCE);
		}

		shader_program_number = GPU_LinkShaders(v, p);

		atlas = texture_atlas_new(2048, 2048, 1);
	}

	ptr = calloc(sizeof(JiveFont), 1);
	
	if (!load_ttf_font(ptr, name, size)) {
		free(ptr);
		return NULL;
	}

	// Texture font
	ptr->t_font = texture_font_new_from_file(atlas, size, name);
	size_t missed = texture_font_load_glyphs(ptr->t_font, font_cache);

	//assert(missed == 0);

	ptr->shader_program_number = shader_program_number;
	ptr->refcount = 1;
	ptr->name = strdup(name);
	ptr->size = size;
	ptr->next = fonts;
	ptr->magic = JIVE_FONT_MAGIC;
	fonts = ptr;

	return ptr;
}

JiveFont *jive_font_ref(JiveFont *font) {
	if (font) {
		assert(font->magic == JIVE_FONT_MAGIC);
		++font->refcount;
	}
	return font;
}

void jive_font_free(JiveFont *font) {
	assert(font && font->magic == JIVE_FONT_MAGIC);

	if (--font->refcount > 0) {
		return;
	}

	if (font == fonts) {
		fonts = font->next;
	}
	else {
		JiveFont *ptr = fonts;
		while (ptr) {
			if (ptr->next == font) {
				ptr->next = font->next;
				break;
			}

			ptr = ptr->next;
		}
	}

	font->destroy(font);
	free(font->name);
	free(font);

	/* Shutdown the TTF api when all fonts are free */
	if (fonts == NULL && TTF_WasInit()) {
		TTF_Quit();
		texture_atlas_delete(atlas);
	}
}

int jive_font_width(JiveFont *font, const char *str) {
	assert(font && font->magic == JIVE_FONT_MAGIC);

	return font->width(font, str);
}

int jive_font_nwidth(JiveFont *font, const char *str, size_t len) {
	char *tmp;

	assert(font && font->magic == JIVE_FONT_MAGIC);

	if (len <= 0) {
		return 0;
	}

	// FIXME use utf8 len
	tmp = alloca(len + 1);
	strncpy(tmp, str, len);
	*(tmp + len) = '\0';

	return font->width(font, tmp);
}

int jive_font_miny_char(JiveFont *font, Uint16 ch) {
	int miny;

	assert(font && font->magic == JIVE_FONT_MAGIC);

	TTF_GlyphMetrics(font->ttf, ch, NULL, NULL, &miny, NULL, NULL);

	return miny;
}

int jive_font_maxy_char(JiveFont *font, Uint16 ch) {
	int maxy;

	assert(font && font->magic == JIVE_FONT_MAGIC);

	TTF_GlyphMetrics(font->ttf, ch, NULL, NULL, NULL, &maxy, NULL);

	return maxy;
}

int jive_font_capheight(JiveFont *font) {
	assert(font && font->magic == JIVE_FONT_MAGIC);

	return font->capheight;
}

int jive_font_height(JiveFont *font) {
	assert(font && font->magic == JIVE_FONT_MAGIC);

	return font->height;
}

int jive_font_ascend(JiveFont *font) {
	assert(font && font->magic == JIVE_FONT_MAGIC);

	return font->ascend;
}

int jive_font_offset(JiveFont *font) {
	assert(font && font->magic == JIVE_FONT_MAGIC);

	return font->ascend - font->capheight;
}

static int load_ttf_font(JiveFont *font, const char *name, Uint16 size) {
	int miny, maxy, descent;
	char *fullpath = malloc(PATH_MAX);

	if (!jive_find_file(name, fullpath) ) {
		free(fullpath);
		LOG_WARN(log_ui_draw, "Cannot find font %s\n", name);
		return 0;
	}

	font->ttf = TTF_OpenFont(fullpath, size);
	if (!font->ttf) {
		free(fullpath);
		LOG_WARN(log_ui_draw, "TTF_OpenFont: %s\n", TTF_GetError());
		return 0;
	}
	free(fullpath);

	font->ascend = TTF_FontAscent(font->ttf);

	/* calcualte the cap height using H */
	if (TTF_GlyphMetrics(font->ttf, 'H', NULL, NULL, NULL, &maxy, NULL) == 0) {
		font->capheight = maxy;
	}
	else {
		font->capheight = font->ascend;
	}

	/* calcualte the non diacritical descent using g */
	if (TTF_GlyphMetrics(font->ttf, 'g', NULL, NULL, &miny, NULL, NULL) == 0) {
		descent = miny;
	}
	else {
		descent = TTF_FontDescent(font->ttf);
	}

	/* calculate the font height, using the capheight and descent */
	font->height = font->capheight - descent + 1;

	font->width = width_ttf_font;
	font->draw = draw_ttf_font;
	font->destroy = destroy_ttf_font;

	return 1;
}

static void destroy_ttf_font(JiveFont *font) {
	if (font->ttf) {
		TTF_CloseFont(font->ttf);
		font->ttf = NULL;
	}

	texture_font_delete(font->t_font);
}

static int width_ttf_font(JiveFont *font, const char *str) {
	int w, h;

	if (!str) {
		return 0;
	}

	TTF_SizeUTF8(font->ttf, str, &w, &h);
	return w;
}

float get_width(JiveDrawText *text) {
	int i;
	float width = 0;

	for (i = 0; i < strlen(text->str); ++i)
	{
		texture_glyph_t *glyph = texture_font_get_glyph(text->font->t_font, text->str + i);

		if (glyph != NULL)
		{
			float kerning = 0.0f;
			if (i > 0)
			{
				kerning = texture_glyph_get_kerning(glyph, text->str + i - 1);
			}
			width += kerning;
			//width += glyph->offset_x;
			//width += glyph->width;
			width += glyph->advance_x;
		}
	}

	return width;
}

static JiveDrawText *draw_ttf_font(JiveFont *font, Uint32 color, const char *str) {
#ifdef JIVE_PROFILE_BLIT
	Uint32 t0 = jive_jiffies(), t1;
#endif //JIVE_PROFILE_BLIT
	//SDL_Color clr;
	//SDL_Surface *srf;
	//GPU_Image *image;

	// don't call render for null strings as it produces an error which we want to hide
	if (*str == '\0') {
		return NULL;
	}

	if (atlas_image == NULL) {
		atlas_image = GPU_CreateImage(atlas->width, atlas->height, GPU_FORMAT_ALPHA);
		atlas_image->anchor_x = 0;
		atlas_image->anchor_y = 0;
		atlas_image->has_mipmaps = false;
		atlas_image->wrap_mode_x = GPU_WRAP_NONE;
		atlas_image->wrap_mode_y = GPU_WRAP_NONE;
		atlas_image->snap_mode = GPU_SNAP_POSITION;

		GPU_UpdateImageBytes(atlas_image, NULL, atlas->data, atlas_image->bytes_per_pixel * atlas_image->w);
	}

	JiveDrawText *text;
	text = calloc(sizeof(JiveDrawText), 1);

	text->font = font;
	text->str = calloc(strlen(str) + 1, sizeof(char));

	strncpy(text->str, str, strlen(str));
	text->str[strlen(str)] = '\0';

	text->atlas_image = atlas_image;

	text->color.r = (color >> 24) & 0xFF;
	text->color.g = (color >> 16) & 0xFF;
	text->color.b = (color >> 8) & 0xFF;
	text->color.a = 255;

	text->width = get_width(text);

	/*atlas_image = GPU_LoadImage_RW(S, 1);

	atlas_image = GPU_CopyImageFromSurface(surface);*/

	//glGenTextures(1, &atlas->id);
	//glBindTexture(GL_TEXTURE_2D, atlas->id);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	//
	//GLint swizzleMask[] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED };
	//glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);

	//glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, atlas->width, atlas->height,
	//	0, GL_ALPHA, GL_UNSIGNED_BYTE, atlas->data);

	//atlas_image = GPU_CreateImageUsingTexture(atlas->id, true);

	

	//srf = TTF_RenderUTF8_Blended(font->ttf, str, clr);
	

	/*if (!srf) {
		LOG_ERROR(log_ui_draw, "render returned error: %s\n", TTF_GetError());
	}*/

#if 0
	// draw text bounding box for debugging
	if (srf) {
		rectangleColor(srf, 0,0, srf->w - 1, srf->h - 1, 0xff0000df);
		lineColor(srf, 0, font->ascend, srf->w - 1, font->ascend, 0xff0000df);
		lineColor(srf, 0, font->ascend, srf->w - 1, font->ascend, 0xff0000df);
		lineColor(srf, 0, font->ascend - font->capheight, srf->w - 1, font->ascend - font->capheight, 0xff0000df);
	}
#endif


#ifdef JIVE_PROFILE_BLIT
	t1 = jive_jiffies();
	printf("\tdraw_ttf_font took=%d %s\n", t1-t0, str);
#endif //JIVE_PROFILE_BLIT

	//SDL_FreeSurface(srf);
	return text;
}



JiveDrawText *jive_font_draw_text(JiveFont *font, Uint32 color, const char *str) {
	assert(font && font->magic == JIVE_FONT_MAGIC);

	return str ? font->draw(font, color, str) : NULL;
	//return jive_surface_new_SDLSurface(str ? font->draw(font, color, str) : NULL);
}

JiveDrawText *jive_font_ndraw_text(JiveFont *font, Uint32 color, const char *str, size_t len) {
	char *tmp;

	// FIXME use utf8 len

	tmp = alloca(len + 1);
	strncpy(tmp, str, len);
	*(tmp + len) = '\0';
	
	return jive_font_draw_text(font, color, tmp);
}


/*
binary 			hex 	decimal 	notes
00000000-01111111 	00-7F 	0-127	 	US-ASCII (single byte)
10000000-10111111 	80-BF 	128-191 	Second, third, or fourth byte of a multi-byte sequence
11000000-11000001 	C0-C1 	192-193 	Overlong encoding: start of a 2-byte sequence, but code point <= 127
11000010-11011111 	C2-DF 	194-223 	Start of 2-byte sequence
11100000-11101111 	E0-EF 	224-239 	Start of 3-byte sequence
11110000-11110100 	F0-F4 	240-244 	Start of 4-byte sequence
11110101-11110111 	F5-F7 	245-247 	Restricted by RFC 3629: start of 4-byte sequence for codepoint above 10FFFF
11111000-11111011 	F8-FB 	248-251 	Restricted by RFC 3629: start of 5-byte sequence
11111100-11111101 	FC-FD 	252-253 	Restricted by RFC 3629: start of 6-byte sequence
11111110-11111111 	FE-FF 	254-255 	Invalid: not defined by original UTF-8 specification
*/

Uint32 utf8_get_char(const char *ptr, const char **nptr)
{
	Uint32 c, v;
	const unsigned char *uptr = (const unsigned char *)ptr;

	c = *uptr++;

	if (c <= 127) {
		/* US-ASCII */
		v = c & 0x7F;
	}
	else if (c <= 191) {
		/* error */
		v = 0xFFFD;
	}
	else if (c <= 223) {
		/* 2-bytes */
		v = (c & 0x1F) << 6;
		c = *uptr++;
		v |= (c & 0x3F);
	}
	else if (c <= 239) {
		/* 3-byte */
		v = (c & 0x0F) << 12;
		c = *uptr++;
		v |= (c & 0x3F) << 6;
		c = *uptr++;
		v |= (c & 0x3F);
	}
	else if (c <= 244) {
		/* 4-byte */
		v = (c & 0x07) << 18;
		c = *uptr++;
		v |= (c & 0x3F) << 12;
		c = *uptr++;
		v |= (c & 0x3F) << 6;
		c = *uptr++;
		v |= (c & 0x3F);
	}
	else {
		/* error */
		v = 0xFFFD;
	}

	if (nptr) {
		*nptr = (const char *)uptr;
	}
	return v;
}


int jiveL_font_load(lua_State *L) {
	/*
	  class
	  fontname
	  size
	*/
	const char *fontname = luaL_checklstring(L, 2, NULL);
	int size = luaL_checkint(L, 3);

	if (fontname && size) {
		JiveFont *font = jive_font_load(fontname, size);
		if (font) {
			JiveFont **p = (JiveFont **)lua_newuserdata(L, sizeof(JiveFont *));
			*p = font;
			luaL_getmetatable(L, "JiveFont");
			lua_setmetatable(L, -2);
			return 1;
		}
	}

	return 0;
}

int jiveL_font_free(lua_State *L) {
	JiveFont *font = *(JiveFont **)lua_touserdata(L, 1);
	if (font) {
		jive_font_free(font);
	}
	return 0;
}

int jiveL_font_width(lua_State *L) {
 	JiveFont *font = *(JiveFont **)lua_touserdata(L, 1);
	const char *str = luaL_checklstring(L, 2, NULL);
	if (font) {
		lua_pushinteger(L, jive_font_width(font, str));
		return 1;
	}
	return 0;
}

int jiveL_font_capheight(lua_State *L) {
 	JiveFont *font = *(JiveFont **)lua_touserdata(L, 1);
	if (font) {
		lua_pushinteger(L, jive_font_capheight(font));
		return 1;
	}
	return 0;
}

int jiveL_font_height(lua_State *L) {
 	JiveFont *font = *(JiveFont **)lua_touserdata(L, 1);
	if (font) {
		lua_pushinteger(L, jive_font_height(font));
		return 1;
	}
	return 0;
}

int jiveL_font_ascend(lua_State *L) {
 	JiveFont *font = *(JiveFont **)lua_touserdata(L, 1);
	if (font) {
		lua_pushinteger(L, jive_font_ascend(font));
		return 1;
	}
	return 0;
}

int jiveL_font_offset(lua_State *L) {
 	JiveFont *font = *(JiveFont **)lua_touserdata(L, 1);
	if (font) {
		lua_pushinteger(L, jive_font_height(font));
		return 1;
	}
	return 0;
}

int jiveL_font_gc(lua_State *L) {
 	JiveFont *font = *(JiveFont **)lua_touserdata(L, 1);
	if (font) {
		jive_font_free(font);
	}
	return 0;
}
