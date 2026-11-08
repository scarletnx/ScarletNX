// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.opengl;

import snx.android.opengl.Gles;

/**
 * GLES 2.0's Java calls, each the runtime's GLES function of its name on the calling thread's current context
 * (snx.android.opengl.Gles). A pointer is a direct buffer, or an array (a buffer's or one given with an offset), read
 * from its position. The constants are Khronos' gl2.h's.
 */
public class GLES20 {
    private static final long[] NO_INTS = {};
    private static final float[] NO_FLOATS = {};

    public GLES20() {}

    public static final int GL_GLES_PROTOTYPES = 1;
    public static final int GL_DEPTH_BUFFER_BIT = 0x00000100;
    public static final int GL_STENCIL_BUFFER_BIT = 0x00000400;
    public static final int GL_COLOR_BUFFER_BIT = 0x00004000;
    public static final int GL_FALSE = 0;
    public static final int GL_TRUE = 1;
    public static final int GL_POINTS = 0;
    public static final int GL_LINES = 1;
    public static final int GL_LINE_LOOP = 2;
    public static final int GL_LINE_STRIP = 3;
    public static final int GL_TRIANGLES = 4;
    public static final int GL_TRIANGLE_STRIP = 5;
    public static final int GL_TRIANGLE_FAN = 6;
    public static final int GL_ZERO = 0;
    public static final int GL_ONE = 1;
    public static final int GL_SRC_COLOR = 0x00000300;
    public static final int GL_ONE_MINUS_SRC_COLOR = 0x00000301;
    public static final int GL_SRC_ALPHA = 0x00000302;
    public static final int GL_ONE_MINUS_SRC_ALPHA = 0x00000303;
    public static final int GL_DST_ALPHA = 0x00000304;
    public static final int GL_ONE_MINUS_DST_ALPHA = 0x00000305;
    public static final int GL_DST_COLOR = 0x00000306;
    public static final int GL_ONE_MINUS_DST_COLOR = 0x00000307;
    public static final int GL_SRC_ALPHA_SATURATE = 0x00000308;
    public static final int GL_FUNC_ADD = 0x00008006;
    public static final int GL_BLEND_EQUATION = 0x00008009;
    public static final int GL_BLEND_EQUATION_RGB = 0x00008009;
    public static final int GL_BLEND_EQUATION_ALPHA = 0x0000883d;
    public static final int GL_FUNC_SUBTRACT = 0x0000800a;
    public static final int GL_FUNC_REVERSE_SUBTRACT = 0x0000800b;
    public static final int GL_BLEND_DST_RGB = 0x000080c8;
    public static final int GL_BLEND_SRC_RGB = 0x000080c9;
    public static final int GL_BLEND_DST_ALPHA = 0x000080ca;
    public static final int GL_BLEND_SRC_ALPHA = 0x000080cb;
    public static final int GL_CONSTANT_COLOR = 0x00008001;
    public static final int GL_ONE_MINUS_CONSTANT_COLOR = 0x00008002;
    public static final int GL_CONSTANT_ALPHA = 0x00008003;
    public static final int GL_ONE_MINUS_CONSTANT_ALPHA = 0x00008004;
    public static final int GL_BLEND_COLOR = 0x00008005;
    public static final int GL_ARRAY_BUFFER = 0x00008892;
    public static final int GL_ELEMENT_ARRAY_BUFFER = 0x00008893;
    public static final int GL_ARRAY_BUFFER_BINDING = 0x00008894;
    public static final int GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x00008895;
    public static final int GL_STREAM_DRAW = 0x000088e0;
    public static final int GL_STATIC_DRAW = 0x000088e4;
    public static final int GL_DYNAMIC_DRAW = 0x000088e8;
    public static final int GL_BUFFER_SIZE = 0x00008764;
    public static final int GL_BUFFER_USAGE = 0x00008765;
    public static final int GL_CURRENT_VERTEX_ATTRIB = 0x00008626;
    public static final int GL_FRONT = 0x00000404;
    public static final int GL_BACK = 0x00000405;
    public static final int GL_FRONT_AND_BACK = 0x00000408;
    public static final int GL_TEXTURE_2D = 0x00000de1;
    public static final int GL_CULL_FACE = 0x00000b44;
    public static final int GL_BLEND = 0x00000be2;
    public static final int GL_DITHER = 0x00000bd0;
    public static final int GL_STENCIL_TEST = 0x00000b90;
    public static final int GL_DEPTH_TEST = 0x00000b71;
    public static final int GL_SCISSOR_TEST = 0x00000c11;
    public static final int GL_POLYGON_OFFSET_FILL = 0x00008037;
    public static final int GL_SAMPLE_ALPHA_TO_COVERAGE = 0x0000809e;
    public static final int GL_SAMPLE_COVERAGE = 0x000080a0;
    public static final int GL_NO_ERROR = 0;
    public static final int GL_INVALID_ENUM = 0x00000500;
    public static final int GL_INVALID_VALUE = 0x00000501;
    public static final int GL_INVALID_OPERATION = 0x00000502;
    public static final int GL_OUT_OF_MEMORY = 0x00000505;
    public static final int GL_CW = 0x00000900;
    public static final int GL_CCW = 0x00000901;
    public static final int GL_LINE_WIDTH = 0x00000b21;
    public static final int GL_ALIASED_POINT_SIZE_RANGE = 0x0000846d;
    public static final int GL_ALIASED_LINE_WIDTH_RANGE = 0x0000846e;
    public static final int GL_CULL_FACE_MODE = 0x00000b45;
    public static final int GL_FRONT_FACE = 0x00000b46;
    public static final int GL_DEPTH_RANGE = 0x00000b70;
    public static final int GL_DEPTH_WRITEMASK = 0x00000b72;
    public static final int GL_DEPTH_CLEAR_VALUE = 0x00000b73;
    public static final int GL_DEPTH_FUNC = 0x00000b74;
    public static final int GL_STENCIL_CLEAR_VALUE = 0x00000b91;
    public static final int GL_STENCIL_FUNC = 0x00000b92;
    public static final int GL_STENCIL_FAIL = 0x00000b94;
    public static final int GL_STENCIL_PASS_DEPTH_FAIL = 0x00000b95;
    public static final int GL_STENCIL_PASS_DEPTH_PASS = 0x00000b96;
    public static final int GL_STENCIL_REF = 0x00000b97;
    public static final int GL_STENCIL_VALUE_MASK = 0x00000b93;
    public static final int GL_STENCIL_WRITEMASK = 0x00000b98;
    public static final int GL_STENCIL_BACK_FUNC = 0x00008800;
    public static final int GL_STENCIL_BACK_FAIL = 0x00008801;
    public static final int GL_STENCIL_BACK_PASS_DEPTH_FAIL = 0x00008802;
    public static final int GL_STENCIL_BACK_PASS_DEPTH_PASS = 0x00008803;
    public static final int GL_STENCIL_BACK_REF = 0x00008ca3;
    public static final int GL_STENCIL_BACK_VALUE_MASK = 0x00008ca4;
    public static final int GL_STENCIL_BACK_WRITEMASK = 0x00008ca5;
    public static final int GL_VIEWPORT = 0x00000ba2;
    public static final int GL_SCISSOR_BOX = 0x00000c10;
    public static final int GL_COLOR_CLEAR_VALUE = 0x00000c22;
    public static final int GL_COLOR_WRITEMASK = 0x00000c23;
    public static final int GL_UNPACK_ALIGNMENT = 0x00000cf5;
    public static final int GL_PACK_ALIGNMENT = 0x00000d05;
    public static final int GL_MAX_TEXTURE_SIZE = 0x00000d33;
    public static final int GL_MAX_VIEWPORT_DIMS = 0x00000d3a;
    public static final int GL_SUBPIXEL_BITS = 0x00000d50;
    public static final int GL_RED_BITS = 0x00000d52;
    public static final int GL_GREEN_BITS = 0x00000d53;
    public static final int GL_BLUE_BITS = 0x00000d54;
    public static final int GL_ALPHA_BITS = 0x00000d55;
    public static final int GL_DEPTH_BITS = 0x00000d56;
    public static final int GL_STENCIL_BITS = 0x00000d57;
    public static final int GL_POLYGON_OFFSET_UNITS = 0x00002a00;
    public static final int GL_POLYGON_OFFSET_FACTOR = 0x00008038;
    public static final int GL_TEXTURE_BINDING_2D = 0x00008069;
    public static final int GL_SAMPLE_BUFFERS = 0x000080a8;
    public static final int GL_SAMPLES = 0x000080a9;
    public static final int GL_SAMPLE_COVERAGE_VALUE = 0x000080aa;
    public static final int GL_SAMPLE_COVERAGE_INVERT = 0x000080ab;
    public static final int GL_NUM_COMPRESSED_TEXTURE_FORMATS = 0x000086a2;
    public static final int GL_COMPRESSED_TEXTURE_FORMATS = 0x000086a3;
    public static final int GL_DONT_CARE = 0x00001100;
    public static final int GL_FASTEST = 0x00001101;
    public static final int GL_NICEST = 0x00001102;
    public static final int GL_GENERATE_MIPMAP_HINT = 0x00008192;
    public static final int GL_BYTE = 0x00001400;
    public static final int GL_UNSIGNED_BYTE = 0x00001401;
    public static final int GL_SHORT = 0x00001402;
    public static final int GL_UNSIGNED_SHORT = 0x00001403;
    public static final int GL_INT = 0x00001404;
    public static final int GL_UNSIGNED_INT = 0x00001405;
    public static final int GL_FLOAT = 0x00001406;
    public static final int GL_FIXED = 0x0000140c;
    public static final int GL_DEPTH_COMPONENT = 0x00001902;
    public static final int GL_ALPHA = 0x00001906;
    public static final int GL_RGB = 0x00001907;
    public static final int GL_RGBA = 0x00001908;
    public static final int GL_LUMINANCE = 0x00001909;
    public static final int GL_LUMINANCE_ALPHA = 0x0000190a;
    public static final int GL_UNSIGNED_SHORT_4_4_4_4 = 0x00008033;
    public static final int GL_UNSIGNED_SHORT_5_5_5_1 = 0x00008034;
    public static final int GL_UNSIGNED_SHORT_5_6_5 = 0x00008363;
    public static final int GL_FRAGMENT_SHADER = 0x00008b30;
    public static final int GL_VERTEX_SHADER = 0x00008b31;
    public static final int GL_MAX_VERTEX_ATTRIBS = 0x00008869;
    public static final int GL_MAX_VERTEX_UNIFORM_VECTORS = 0x00008dfb;
    public static final int GL_MAX_VARYING_VECTORS = 0x00008dfc;
    public static final int GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x00008b4d;
    public static final int GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0x00008b4c;
    public static final int GL_MAX_TEXTURE_IMAGE_UNITS = 0x00008872;
    public static final int GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x00008dfd;
    public static final int GL_SHADER_TYPE = 0x00008b4f;
    public static final int GL_DELETE_STATUS = 0x00008b80;
    public static final int GL_LINK_STATUS = 0x00008b82;
    public static final int GL_VALIDATE_STATUS = 0x00008b83;
    public static final int GL_ATTACHED_SHADERS = 0x00008b85;
    public static final int GL_ACTIVE_UNIFORMS = 0x00008b86;
    public static final int GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x00008b87;
    public static final int GL_ACTIVE_ATTRIBUTES = 0x00008b89;
    public static final int GL_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x00008b8a;
    public static final int GL_SHADING_LANGUAGE_VERSION = 0x00008b8c;
    public static final int GL_CURRENT_PROGRAM = 0x00008b8d;
    public static final int GL_NEVER = 0x00000200;
    public static final int GL_LESS = 0x00000201;
    public static final int GL_EQUAL = 0x00000202;
    public static final int GL_LEQUAL = 0x00000203;
    public static final int GL_GREATER = 0x00000204;
    public static final int GL_NOTEQUAL = 0x00000205;
    public static final int GL_GEQUAL = 0x00000206;
    public static final int GL_ALWAYS = 0x00000207;
    public static final int GL_KEEP = 0x00001e00;
    public static final int GL_REPLACE = 0x00001e01;
    public static final int GL_INCR = 0x00001e02;
    public static final int GL_DECR = 0x00001e03;
    public static final int GL_INVERT = 0x0000150a;
    public static final int GL_INCR_WRAP = 0x00008507;
    public static final int GL_DECR_WRAP = 0x00008508;
    public static final int GL_VENDOR = 0x00001f00;
    public static final int GL_RENDERER = 0x00001f01;
    public static final int GL_VERSION = 0x00001f02;
    public static final int GL_EXTENSIONS = 0x00001f03;
    public static final int GL_NEAREST = 0x00002600;
    public static final int GL_LINEAR = 0x00002601;
    public static final int GL_NEAREST_MIPMAP_NEAREST = 0x00002700;
    public static final int GL_LINEAR_MIPMAP_NEAREST = 0x00002701;
    public static final int GL_NEAREST_MIPMAP_LINEAR = 0x00002702;
    public static final int GL_LINEAR_MIPMAP_LINEAR = 0x00002703;
    public static final int GL_TEXTURE_MAG_FILTER = 0x00002800;
    public static final int GL_TEXTURE_MIN_FILTER = 0x00002801;
    public static final int GL_TEXTURE_WRAP_S = 0x00002802;
    public static final int GL_TEXTURE_WRAP_T = 0x00002803;
    public static final int GL_TEXTURE = 0x00001702;
    public static final int GL_TEXTURE_CUBE_MAP = 0x00008513;
    public static final int GL_TEXTURE_BINDING_CUBE_MAP = 0x00008514;
    public static final int GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x00008515;
    public static final int GL_TEXTURE_CUBE_MAP_NEGATIVE_X = 0x00008516;
    public static final int GL_TEXTURE_CUBE_MAP_POSITIVE_Y = 0x00008517;
    public static final int GL_TEXTURE_CUBE_MAP_NEGATIVE_Y = 0x00008518;
    public static final int GL_TEXTURE_CUBE_MAP_POSITIVE_Z = 0x00008519;
    public static final int GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x0000851a;
    public static final int GL_MAX_CUBE_MAP_TEXTURE_SIZE = 0x0000851c;
    public static final int GL_TEXTURE0 = 0x000084c0;
    public static final int GL_TEXTURE1 = 0x000084c1;
    public static final int GL_TEXTURE2 = 0x000084c2;
    public static final int GL_TEXTURE3 = 0x000084c3;
    public static final int GL_TEXTURE4 = 0x000084c4;
    public static final int GL_TEXTURE5 = 0x000084c5;
    public static final int GL_TEXTURE6 = 0x000084c6;
    public static final int GL_TEXTURE7 = 0x000084c7;
    public static final int GL_TEXTURE8 = 0x000084c8;
    public static final int GL_TEXTURE9 = 0x000084c9;
    public static final int GL_TEXTURE10 = 0x000084ca;
    public static final int GL_TEXTURE11 = 0x000084cb;
    public static final int GL_TEXTURE12 = 0x000084cc;
    public static final int GL_TEXTURE13 = 0x000084cd;
    public static final int GL_TEXTURE14 = 0x000084ce;
    public static final int GL_TEXTURE15 = 0x000084cf;
    public static final int GL_TEXTURE16 = 0x000084d0;
    public static final int GL_TEXTURE17 = 0x000084d1;
    public static final int GL_TEXTURE18 = 0x000084d2;
    public static final int GL_TEXTURE19 = 0x000084d3;
    public static final int GL_TEXTURE20 = 0x000084d4;
    public static final int GL_TEXTURE21 = 0x000084d5;
    public static final int GL_TEXTURE22 = 0x000084d6;
    public static final int GL_TEXTURE23 = 0x000084d7;
    public static final int GL_TEXTURE24 = 0x000084d8;
    public static final int GL_TEXTURE25 = 0x000084d9;
    public static final int GL_TEXTURE26 = 0x000084da;
    public static final int GL_TEXTURE27 = 0x000084db;
    public static final int GL_TEXTURE28 = 0x000084dc;
    public static final int GL_TEXTURE29 = 0x000084dd;
    public static final int GL_TEXTURE30 = 0x000084de;
    public static final int GL_TEXTURE31 = 0x000084df;
    public static final int GL_ACTIVE_TEXTURE = 0x000084e0;
    public static final int GL_REPEAT = 0x00002901;
    public static final int GL_CLAMP_TO_EDGE = 0x0000812f;
    public static final int GL_MIRRORED_REPEAT = 0x00008370;
    public static final int GL_FLOAT_VEC2 = 0x00008b50;
    public static final int GL_FLOAT_VEC3 = 0x00008b51;
    public static final int GL_FLOAT_VEC4 = 0x00008b52;
    public static final int GL_INT_VEC2 = 0x00008b53;
    public static final int GL_INT_VEC3 = 0x00008b54;
    public static final int GL_INT_VEC4 = 0x00008b55;
    public static final int GL_BOOL = 0x00008b56;
    public static final int GL_BOOL_VEC2 = 0x00008b57;
    public static final int GL_BOOL_VEC3 = 0x00008b58;
    public static final int GL_BOOL_VEC4 = 0x00008b59;
    public static final int GL_FLOAT_MAT2 = 0x00008b5a;
    public static final int GL_FLOAT_MAT3 = 0x00008b5b;
    public static final int GL_FLOAT_MAT4 = 0x00008b5c;
    public static final int GL_SAMPLER_2D = 0x00008b5e;
    public static final int GL_SAMPLER_CUBE = 0x00008b60;
    public static final int GL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x00008622;
    public static final int GL_VERTEX_ATTRIB_ARRAY_SIZE = 0x00008623;
    public static final int GL_VERTEX_ATTRIB_ARRAY_STRIDE = 0x00008624;
    public static final int GL_VERTEX_ATTRIB_ARRAY_TYPE = 0x00008625;
    public static final int GL_VERTEX_ATTRIB_ARRAY_NORMALIZED = 0x0000886a;
    public static final int GL_VERTEX_ATTRIB_ARRAY_POINTER = 0x00008645;
    public static final int GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING = 0x0000889f;
    public static final int GL_IMPLEMENTATION_COLOR_READ_TYPE = 0x00008b9a;
    public static final int GL_IMPLEMENTATION_COLOR_READ_FORMAT = 0x00008b9b;
    public static final int GL_COMPILE_STATUS = 0x00008b81;
    public static final int GL_INFO_LOG_LENGTH = 0x00008b84;
    public static final int GL_SHADER_SOURCE_LENGTH = 0x00008b88;
    public static final int GL_SHADER_COMPILER = 0x00008dfa;
    public static final int GL_SHADER_BINARY_FORMATS = 0x00008df8;
    public static final int GL_NUM_SHADER_BINARY_FORMATS = 0x00008df9;
    public static final int GL_LOW_FLOAT = 0x00008df0;
    public static final int GL_MEDIUM_FLOAT = 0x00008df1;
    public static final int GL_HIGH_FLOAT = 0x00008df2;
    public static final int GL_LOW_INT = 0x00008df3;
    public static final int GL_MEDIUM_INT = 0x00008df4;
    public static final int GL_HIGH_INT = 0x00008df5;
    public static final int GL_FRAMEBUFFER = 0x00008d40;
    public static final int GL_RENDERBUFFER = 0x00008d41;
    public static final int GL_RGBA4 = 0x00008056;
    public static final int GL_RGB5_A1 = 0x00008057;
    public static final int GL_RGB565 = 0x00008d62;
    public static final int GL_DEPTH_COMPONENT16 = 0x000081a5;
    public static final int GL_STENCIL_INDEX8 = 0x00008d48;
    public static final int GL_RENDERBUFFER_WIDTH = 0x00008d42;
    public static final int GL_RENDERBUFFER_HEIGHT = 0x00008d43;
    public static final int GL_RENDERBUFFER_INTERNAL_FORMAT = 0x00008d44;
    public static final int GL_RENDERBUFFER_RED_SIZE = 0x00008d50;
    public static final int GL_RENDERBUFFER_GREEN_SIZE = 0x00008d51;
    public static final int GL_RENDERBUFFER_BLUE_SIZE = 0x00008d52;
    public static final int GL_RENDERBUFFER_ALPHA_SIZE = 0x00008d53;
    public static final int GL_RENDERBUFFER_DEPTH_SIZE = 0x00008d54;
    public static final int GL_RENDERBUFFER_STENCIL_SIZE = 0x00008d55;
    public static final int GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE = 0x00008cd0;
    public static final int GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME = 0x00008cd1;
    public static final int GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL = 0x00008cd2;
    public static final int GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE = 0x00008cd3;
    public static final int GL_COLOR_ATTACHMENT0 = 0x00008ce0;
    public static final int GL_DEPTH_ATTACHMENT = 0x00008d00;
    public static final int GL_STENCIL_ATTACHMENT = 0x00008d20;
    public static final int GL_NONE = 0;
    public static final int GL_FRAMEBUFFER_COMPLETE = 0x00008cd5;
    public static final int GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT = 0x00008cd6;
    public static final int GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT = 0x00008cd7;
    public static final int GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS = 0x00008cd9;
    public static final int GL_FRAMEBUFFER_UNSUPPORTED = 0x00008cdd;
    public static final int GL_FRAMEBUFFER_BINDING = 0x00008ca6;
    public static final int GL_RENDERBUFFER_BINDING = 0x00008ca7;
    public static final int GL_MAX_RENDERBUFFER_SIZE = 0x000084e8;
    public static final int GL_INVALID_FRAMEBUFFER_OPERATION = 0x00000506;

    /** glShaderSource(shader, string): the one string, as an array of one with no lengths */
    public static void glShaderSource(int shader, String string) {
        Gles.call("glShaderSource", "iiSi", new long[] {shader, 1, 0}, NO_FLOATS, new Object[] {string}, new int[] {0});
    }
    public static String glGetString(int name) { return Gles.string(name); }
    public static String glGetShaderInfoLog(int shader) { return infoLog("glGetShaderiv", "glGetShaderInfoLog", shader); }
    public static String glGetProgramInfoLog(int program) { return infoLog("glGetProgramiv", "glGetProgramInfoLog", program); }
    /** an info log: its length (GL_INFO_LOG_LENGTH), then its text */
    private static String infoLog(String query, String get, int object) {
        int[] length = new int[1];
        Gles.call(query, "iip", new long[] {object, GL_INFO_LOG_LENGTH}, NO_FLOATS, new Object[] {length}, new int[] {0});
        if (length[0] <= 0) return "";
        byte[] text = new byte[length[0]];
        int[] written = new int[1];
        Gles.call(get, "iipp", new long[] {object, length[0]}, NO_FLOATS, new Object[] {written, text}, new int[] {0, 0});
        int n = Math.max(0, Math.min(written[0], text.length));
        return new String(text, 0, n, java.nio.charset.StandardCharsets.UTF_8);
    }

    public static void glActiveTexture(int a0) { Gles.call("glActiveTexture", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glAttachShader(int a0, int a1) { Gles.call("glAttachShader", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glBindAttribLocation(int a0, int a1, String a2) { Gles.call("glBindAttribLocation", "iis", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {0}); }
    public static void glBindBuffer(int a0, int a1) { Gles.call("glBindBuffer", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glBindFramebuffer(int a0, int a1) { Gles.call("glBindFramebuffer", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glBindRenderbuffer(int a0, int a1) { Gles.call("glBindRenderbuffer", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glBindTexture(int a0, int a1) { Gles.call("glBindTexture", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glBlendColor(float a0, float a1, float a2, float a3) { Gles.call("glBlendColor", "ffff", NO_INTS, new float[] {a0, a1, a2, a3}, null, null); }
    public static void glBlendEquation(int a0) { Gles.call("glBlendEquation", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glBlendEquationSeparate(int a0, int a1) { Gles.call("glBlendEquationSeparate", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glBlendFunc(int a0, int a1) { Gles.call("glBlendFunc", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glBlendFuncSeparate(int a0, int a1, int a2, int a3) { Gles.call("glBlendFuncSeparate", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glBufferData(int a0, int a1, java.nio.Buffer a2, int a3) { Gles.call("glBufferData", "iipi", new long[] {a0, a1, a3}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glBufferSubData(int a0, int a1, int a2, java.nio.Buffer a3) { Gles.call("glBufferSubData", "iiip", new long[] {a0, a1, a2}, NO_FLOATS, new Object[] {Gles.base(a3)}, new int[] {Gles.offset(a3)}); }
    public static int glCheckFramebufferStatus(int a0) { return (int) Gles.call("glCheckFramebufferStatus", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glClear(int a0) { Gles.call("glClear", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glClearColor(float a0, float a1, float a2, float a3) { Gles.call("glClearColor", "ffff", NO_INTS, new float[] {a0, a1, a2, a3}, null, null); }
    public static void glClearDepthf(float a0) { Gles.call("glClearDepthf", "f", NO_INTS, new float[] {a0}, null, null); }
    public static void glClearStencil(int a0) { Gles.call("glClearStencil", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glColorMask(boolean a0, boolean a1, boolean a2, boolean a3) { Gles.call("glColorMask", "iiii", new long[] {(a0 ? 1 : 0), (a1 ? 1 : 0), (a2 ? 1 : 0), (a3 ? 1 : 0)}, NO_FLOATS, null, null); }
    public static void glCompileShader(int a0) { Gles.call("glCompileShader", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glCompressedTexImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, java.nio.Buffer a7) { Gles.call("glCompressedTexImage2D", "iiiiiiip", new long[] {a0, a1, a2, a3, a4, a5, a6}, NO_FLOATS, new Object[] {Gles.base(a7)}, new int[] {Gles.offset(a7)}); }
    public static void glCompressedTexSubImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, java.nio.Buffer a8) { Gles.call("glCompressedTexSubImage2D", "iiiiiiiip", new long[] {a0, a1, a2, a3, a4, a5, a6, a7}, NO_FLOATS, new Object[] {Gles.base(a8)}, new int[] {Gles.offset(a8)}); }
    public static void glCopyTexImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) { Gles.call("glCopyTexImage2D", "iiiiiiii", new long[] {a0, a1, a2, a3, a4, a5, a6, a7}, NO_FLOATS, null, null); }
    public static void glCopyTexSubImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7) { Gles.call("glCopyTexSubImage2D", "iiiiiiii", new long[] {a0, a1, a2, a3, a4, a5, a6, a7}, NO_FLOATS, null, null); }
    public static int glCreateProgram() { return (int) Gles.call("glCreateProgram", "", NO_INTS, NO_FLOATS, null, null); }
    public static int glCreateShader(int a0) { return (int) Gles.call("glCreateShader", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glCullFace(int a0) { Gles.call("glCullFace", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glDeleteBuffers(int a0, java.nio.IntBuffer a1) { Gles.call("glDeleteBuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glDeleteBuffers(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glDeleteBuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glDeleteFramebuffers(int a0, java.nio.IntBuffer a1) { Gles.call("glDeleteFramebuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glDeleteFramebuffers(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glDeleteFramebuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glDeleteProgram(int a0) { Gles.call("glDeleteProgram", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glDeleteRenderbuffers(int a0, java.nio.IntBuffer a1) { Gles.call("glDeleteRenderbuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glDeleteRenderbuffers(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glDeleteRenderbuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glDeleteShader(int a0) { Gles.call("glDeleteShader", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glDeleteTextures(int a0, java.nio.IntBuffer a1) { Gles.call("glDeleteTextures", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glDeleteTextures(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glDeleteTextures", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glDepthFunc(int a0) { Gles.call("glDepthFunc", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glDepthMask(boolean a0) { Gles.call("glDepthMask", "i", new long[] {(a0 ? 1 : 0)}, NO_FLOATS, null, null); }
    public static void glDepthRangef(float a0, float a1) { Gles.call("glDepthRangef", "ff", NO_INTS, new float[] {a0, a1}, null, null); }
    public static void glDetachShader(int a0, int a1) { Gles.call("glDetachShader", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glDisable(int a0) { Gles.call("glDisable", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glDisableVertexAttribArray(int a0) { Gles.call("glDisableVertexAttribArray", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glDrawArrays(int a0, int a1, int a2) { Gles.call("glDrawArrays", "iii", new long[] {a0, a1, a2}, NO_FLOATS, null, null); }
    public static void glDrawElements(int a0, int a1, int a2, int a3) { Gles.call("glDrawElements", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glDrawElements(int a0, int a1, int a2, java.nio.Buffer a3) { Gles.call("glDrawElements", "iiip", new long[] {a0, a1, a2}, NO_FLOATS, new Object[] {Gles.base(a3)}, new int[] {Gles.offset(a3)}); }
    public static void glEnable(int a0) { Gles.call("glEnable", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glEnableVertexAttribArray(int a0) { Gles.call("glEnableVertexAttribArray", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glFinish() { Gles.call("glFinish", "", NO_INTS, NO_FLOATS, null, null); }
    public static void glFlush() { Gles.call("glFlush", "", NO_INTS, NO_FLOATS, null, null); }
    public static void glFramebufferRenderbuffer(int a0, int a1, int a2, int a3) { Gles.call("glFramebufferRenderbuffer", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glFramebufferTexture2D(int a0, int a1, int a2, int a3, int a4) { Gles.call("glFramebufferTexture2D", "iiiii", new long[] {a0, a1, a2, a3, a4}, NO_FLOATS, null, null); }
    public static void glFrontFace(int a0) { Gles.call("glFrontFace", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glGenBuffers(int a0, java.nio.IntBuffer a1) { Gles.call("glGenBuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glGenBuffers(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glGenBuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glGenFramebuffers(int a0, java.nio.IntBuffer a1) { Gles.call("glGenFramebuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glGenFramebuffers(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glGenFramebuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glGenRenderbuffers(int a0, java.nio.IntBuffer a1) { Gles.call("glGenRenderbuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glGenRenderbuffers(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glGenRenderbuffers", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glGenTextures(int a0, java.nio.IntBuffer a1) { Gles.call("glGenTextures", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glGenTextures(int a0, int[] a1, int a2) { Gles.checkArray(a1, a2); Gles.call("glGenTextures", "ip", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {a2 << 2}); }
    public static void glGenerateMipmap(int a0) { Gles.call("glGenerateMipmap", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glGetActiveAttrib(int a0, int a1, int a2, int[] a3, int a4, int[] a5, int a6, int[] a7, int a8, byte[] a9, int a10) { Gles.checkArray(a3, a4); Gles.checkArray(a5, a6); Gles.checkArray(a7, a8); Gles.checkArray(a9, a10); Gles.call("glGetActiveAttrib", "iiipppp", new long[] {a0, a1, a2}, NO_FLOATS, new Object[] {a3, a5, a7, a9}, new int[] {a4 << 2, a6 << 2, a8 << 2, a10}); }
    public static void glGetActiveUniform(int a0, int a1, int a2, int[] a3, int a4, int[] a5, int a6, int[] a7, int a8, byte[] a9, int a10) { Gles.checkArray(a3, a4); Gles.checkArray(a5, a6); Gles.checkArray(a7, a8); Gles.checkArray(a9, a10); Gles.call("glGetActiveUniform", "iiipppp", new long[] {a0, a1, a2}, NO_FLOATS, new Object[] {a3, a5, a7, a9}, new int[] {a4 << 2, a6 << 2, a8 << 2, a10}); }
    public static void glGetAttachedShaders(int a0, int a1, java.nio.IntBuffer a2, java.nio.IntBuffer a3) { Gles.call("glGetAttachedShaders", "iipp", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2), Gles.base(a3)}, new int[] {Gles.offset(a2), Gles.offset(a3)}); }
    public static int glGetAttribLocation(int a0, String a1) { return (int) Gles.call("glGetAttribLocation", "is", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {0}); }
    public static void glGetBooleanv(int a0, java.nio.IntBuffer a1) { Gles.call("glGetBooleanv", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glGetBufferParameteriv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glGetBufferParameteriv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static int glGetError() { return (int) Gles.call("glGetError", "", NO_INTS, NO_FLOATS, null, null); }
    public static void glGetFloatv(int a0, java.nio.FloatBuffer a1) { Gles.call("glGetFloatv", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glGetFramebufferAttachmentParameteriv(int a0, int a1, int a2, java.nio.IntBuffer a3) { Gles.call("glGetFramebufferAttachmentParameteriv", "iiip", new long[] {a0, a1, a2}, NO_FLOATS, new Object[] {Gles.base(a3)}, new int[] {Gles.offset(a3)}); }
    public static void glGetIntegerv(int a0, java.nio.IntBuffer a1) { Gles.call("glGetIntegerv", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glGetProgramiv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glGetProgramiv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glGetRenderbufferParameteriv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glGetRenderbufferParameteriv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glGetShaderPrecisionFormat(int a0, int a1, java.nio.IntBuffer a2, java.nio.IntBuffer a3) { Gles.call("glGetShaderPrecisionFormat", "iipp", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2), Gles.base(a3)}, new int[] {Gles.offset(a2), Gles.offset(a3)}); }
    public static void glGetShaderiv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glGetShaderiv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glGetTexParameterfv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glGetTexParameterfv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glGetTexParameteriv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glGetTexParameteriv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static int glGetUniformLocation(int a0, String a1) { return (int) Gles.call("glGetUniformLocation", "is", new long[] {a0}, NO_FLOATS, new Object[] {a1}, new int[] {0}); }
    public static void glGetUniformfv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glGetUniformfv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glGetUniformiv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glGetUniformiv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glGetVertexAttribfv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glGetVertexAttribfv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glGetVertexAttribiv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glGetVertexAttribiv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glHint(int a0, int a1) { Gles.call("glHint", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static boolean glIsBuffer(int a0) { return Gles.call("glIsBuffer", "i", new long[] {a0}, NO_FLOATS, null, null) != 0; }
    public static boolean glIsEnabled(int a0) { return Gles.call("glIsEnabled", "i", new long[] {a0}, NO_FLOATS, null, null) != 0; }
    public static boolean glIsFramebuffer(int a0) { return Gles.call("glIsFramebuffer", "i", new long[] {a0}, NO_FLOATS, null, null) != 0; }
    public static boolean glIsProgram(int a0) { return Gles.call("glIsProgram", "i", new long[] {a0}, NO_FLOATS, null, null) != 0; }
    public static boolean glIsRenderbuffer(int a0) { return Gles.call("glIsRenderbuffer", "i", new long[] {a0}, NO_FLOATS, null, null) != 0; }
    public static boolean glIsShader(int a0) { return Gles.call("glIsShader", "i", new long[] {a0}, NO_FLOATS, null, null) != 0; }
    public static boolean glIsTexture(int a0) { return Gles.call("glIsTexture", "i", new long[] {a0}, NO_FLOATS, null, null) != 0; }
    public static void glLineWidth(float a0) { Gles.call("glLineWidth", "f", NO_INTS, new float[] {a0}, null, null); }
    public static void glLinkProgram(int a0) { Gles.call("glLinkProgram", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glPixelStorei(int a0, int a1) { Gles.call("glPixelStorei", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glPolygonOffset(float a0, float a1) { Gles.call("glPolygonOffset", "ff", NO_INTS, new float[] {a0, a1}, null, null); }
    public static void glReadPixels(int a0, int a1, int a2, int a3, int a4, int a5, java.nio.Buffer a6) { Gles.call("glReadPixels", "iiiiiip", new long[] {a0, a1, a2, a3, a4, a5}, NO_FLOATS, new Object[] {Gles.base(a6)}, new int[] {Gles.offset(a6)}); }
    public static void glReleaseShaderCompiler() { Gles.call("glReleaseShaderCompiler", "", NO_INTS, NO_FLOATS, null, null); }
    public static void glRenderbufferStorage(int a0, int a1, int a2, int a3) { Gles.call("glRenderbufferStorage", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glSampleCoverage(float a0, boolean a1) { Gles.call("glSampleCoverage", "fi", new long[] {(a1 ? 1 : 0)}, new float[] {a0}, null, null); }
    public static void glScissor(int a0, int a1, int a2, int a3) { Gles.call("glScissor", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glShaderBinary(int a0, java.nio.IntBuffer a1, int a2, java.nio.Buffer a3, int a4) { Gles.call("glShaderBinary", "ipipi", new long[] {a0, a2, a4}, NO_FLOATS, new Object[] {Gles.base(a1), Gles.base(a3)}, new int[] {Gles.offset(a1), Gles.offset(a3)}); }
    public static void glStencilFunc(int a0, int a1, int a2) { Gles.call("glStencilFunc", "iii", new long[] {a0, a1, a2}, NO_FLOATS, null, null); }
    public static void glStencilFuncSeparate(int a0, int a1, int a2, int a3) { Gles.call("glStencilFuncSeparate", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glStencilMask(int a0) { Gles.call("glStencilMask", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glStencilMaskSeparate(int a0, int a1) { Gles.call("glStencilMaskSeparate", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glStencilOp(int a0, int a1, int a2) { Gles.call("glStencilOp", "iii", new long[] {a0, a1, a2}, NO_FLOATS, null, null); }
    public static void glStencilOpSeparate(int a0, int a1, int a2, int a3) { Gles.call("glStencilOpSeparate", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glTexImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, java.nio.Buffer a8) { Gles.call("glTexImage2D", "iiiiiiiip", new long[] {a0, a1, a2, a3, a4, a5, a6, a7}, NO_FLOATS, new Object[] {Gles.base(a8)}, new int[] {Gles.offset(a8)}); }
    public static void glTexParameterf(int a0, int a1, float a2) { Gles.call("glTexParameterf", "iif", new long[] {a0, a1}, new float[] {a2}, null, null); }
    public static void glTexParameterfv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glTexParameterfv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glTexParameteri(int a0, int a1, int a2) { Gles.call("glTexParameteri", "iii", new long[] {a0, a1, a2}, NO_FLOATS, null, null); }
    public static void glTexParameteriv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glTexParameteriv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glTexSubImage2D(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, java.nio.Buffer a8) { Gles.call("glTexSubImage2D", "iiiiiiiip", new long[] {a0, a1, a2, a3, a4, a5, a6, a7}, NO_FLOATS, new Object[] {Gles.base(a8)}, new int[] {Gles.offset(a8)}); }
    public static void glUniform1f(int a0, float a1) { Gles.call("glUniform1f", "if", new long[] {a0}, new float[] {a1}, null, null); }
    public static void glUniform1fv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glUniform1fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform1fv(int a0, int a1, float[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform1fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniform1i(int a0, int a1) { Gles.call("glUniform1i", "ii", new long[] {a0, a1}, NO_FLOATS, null, null); }
    public static void glUniform1iv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glUniform1iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform1iv(int a0, int a1, int[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform1iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniform2f(int a0, float a1, float a2) { Gles.call("glUniform2f", "iff", new long[] {a0}, new float[] {a1, a2}, null, null); }
    public static void glUniform2fv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glUniform2fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform2fv(int a0, int a1, float[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform2fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniform2i(int a0, int a1, int a2) { Gles.call("glUniform2i", "iii", new long[] {a0, a1, a2}, NO_FLOATS, null, null); }
    public static void glUniform2iv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glUniform2iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform2iv(int a0, int a1, int[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform2iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniform3f(int a0, float a1, float a2, float a3) { Gles.call("glUniform3f", "ifff", new long[] {a0}, new float[] {a1, a2, a3}, null, null); }
    public static void glUniform3fv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glUniform3fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform3fv(int a0, int a1, float[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform3fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniform3i(int a0, int a1, int a2, int a3) { Gles.call("glUniform3i", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
    public static void glUniform3iv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glUniform3iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform3iv(int a0, int a1, int[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform3iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniform4f(int a0, float a1, float a2, float a3, float a4) { Gles.call("glUniform4f", "iffff", new long[] {a0}, new float[] {a1, a2, a3, a4}, null, null); }
    public static void glUniform4fv(int a0, int a1, java.nio.FloatBuffer a2) { Gles.call("glUniform4fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform4fv(int a0, int a1, float[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform4fv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniform4i(int a0, int a1, int a2, int a3, int a4) { Gles.call("glUniform4i", "iiiii", new long[] {a0, a1, a2, a3, a4}, NO_FLOATS, null, null); }
    public static void glUniform4iv(int a0, int a1, java.nio.IntBuffer a2) { Gles.call("glUniform4iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {Gles.base(a2)}, new int[] {Gles.offset(a2)}); }
    public static void glUniform4iv(int a0, int a1, int[] a2, int a3) { Gles.checkArray(a2, a3); Gles.call("glUniform4iv", "iip", new long[] {a0, a1}, NO_FLOATS, new Object[] {a2}, new int[] {a3 << 2}); }
    public static void glUniformMatrix2fv(int a0, int a1, boolean a2, java.nio.FloatBuffer a3) { Gles.call("glUniformMatrix2fv", "iiip", new long[] {a0, a1, (a2 ? 1 : 0)}, NO_FLOATS, new Object[] {Gles.base(a3)}, new int[] {Gles.offset(a3)}); }
    public static void glUniformMatrix2fv(int a0, int a1, boolean a2, float[] a3, int a4) { Gles.checkArray(a3, a4); Gles.call("glUniformMatrix2fv", "iiip", new long[] {a0, a1, (a2 ? 1 : 0)}, NO_FLOATS, new Object[] {a3}, new int[] {a4 << 2}); }
    public static void glUniformMatrix3fv(int a0, int a1, boolean a2, java.nio.FloatBuffer a3) { Gles.call("glUniformMatrix3fv", "iiip", new long[] {a0, a1, (a2 ? 1 : 0)}, NO_FLOATS, new Object[] {Gles.base(a3)}, new int[] {Gles.offset(a3)}); }
    public static void glUniformMatrix3fv(int a0, int a1, boolean a2, float[] a3, int a4) { Gles.checkArray(a3, a4); Gles.call("glUniformMatrix3fv", "iiip", new long[] {a0, a1, (a2 ? 1 : 0)}, NO_FLOATS, new Object[] {a3}, new int[] {a4 << 2}); }
    public static void glUniformMatrix4fv(int a0, int a1, boolean a2, java.nio.FloatBuffer a3) { Gles.call("glUniformMatrix4fv", "iiip", new long[] {a0, a1, (a2 ? 1 : 0)}, NO_FLOATS, new Object[] {Gles.base(a3)}, new int[] {Gles.offset(a3)}); }
    public static void glUniformMatrix4fv(int a0, int a1, boolean a2, float[] a3, int a4) { Gles.checkArray(a3, a4); Gles.call("glUniformMatrix4fv", "iiip", new long[] {a0, a1, (a2 ? 1 : 0)}, NO_FLOATS, new Object[] {a3}, new int[] {a4 << 2}); }
    public static void glUseProgram(int a0) { Gles.call("glUseProgram", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glValidateProgram(int a0) { Gles.call("glValidateProgram", "i", new long[] {a0}, NO_FLOATS, null, null); }
    public static void glVertexAttrib1f(int a0, float a1) { Gles.call("glVertexAttrib1f", "if", new long[] {a0}, new float[] {a1}, null, null); }
    public static void glVertexAttrib1fv(int a0, java.nio.FloatBuffer a1) { Gles.call("glVertexAttrib1fv", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glVertexAttrib2f(int a0, float a1, float a2) { Gles.call("glVertexAttrib2f", "iff", new long[] {a0}, new float[] {a1, a2}, null, null); }
    public static void glVertexAttrib2fv(int a0, java.nio.FloatBuffer a1) { Gles.call("glVertexAttrib2fv", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glVertexAttrib3f(int a0, float a1, float a2, float a3) { Gles.call("glVertexAttrib3f", "ifff", new long[] {a0}, new float[] {a1, a2, a3}, null, null); }
    public static void glVertexAttrib3fv(int a0, java.nio.FloatBuffer a1) { Gles.call("glVertexAttrib3fv", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glVertexAttrib4f(int a0, float a1, float a2, float a3, float a4) { Gles.call("glVertexAttrib4f", "iffff", new long[] {a0}, new float[] {a1, a2, a3, a4}, null, null); }
    public static void glVertexAttrib4fv(int a0, java.nio.FloatBuffer a1) { Gles.call("glVertexAttrib4fv", "ip", new long[] {a0}, NO_FLOATS, new Object[] {Gles.base(a1)}, new int[] {Gles.offset(a1)}); }
    public static void glVertexAttribPointer(int a0, int a1, int a2, boolean a3, int a4, int a5) { Gles.call("glVertexAttribPointer", "iiiiii", new long[] {a0, a1, a2, (a3 ? 1 : 0), a4, a5}, NO_FLOATS, null, null); }
    public static void glVertexAttribPointer(int a0, int a1, int a2, boolean a3, int a4, java.nio.Buffer a5) { Gles.call("glVertexAttribPointer", "iiiiip", new long[] {a0, a1, a2, (a3 ? 1 : 0), a4}, NO_FLOATS, new Object[] {Gles.base(a5)}, new int[] {Gles.offset(a5)}); }
    public static void glViewport(int a0, int a1, int a2, int a3) { Gles.call("glViewport", "iiii", new long[] {a0, a1, a2, a3}, NO_FLOATS, null, null); }
}
