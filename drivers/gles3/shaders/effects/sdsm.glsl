/* clang-format off */
#[modes]
mode_sample = #define MODE_SAMPLE
mode_reduce = #define MODE_REDUCE
#[vertex]
layout(location = 0) in vec2 vertex_attrib;
void main() {
	gl_Position = vec4(vertex_attrib, 0.0, 1.0);
}
#[fragment]
/* clang-format on */
uniform highp sampler2D source_texture; // texunit:0
uniform mat4 inverse_projection;
uniform mat4 view_to_camera;
uniform mat4 camera_to_light;
uniform vec2 partition;
uniform int operation;
layout(location = 0) out highp vec4 frag_color;
void main() {
	ivec2 size = textureSize(source_texture, 0);
	ivec2 begin = ivec2(gl_FragCoord.xy) * 4;
	vec4 result = vec4(3.402823e38);
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			ivec2 pixel = begin + ivec2(x, y);
			if (any(greaterThanEqual(pixel, size))) {
				continue;
			}
#ifdef MODE_SAMPLE
			float depth = texelFetch(source_texture, pixel, 0).r;
			if (depth <= 0.0) {
				continue;
			}
			vec4 position = inverse_projection * vec4((vec2(pixel) + 0.5) / vec2(size) * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
			position /= position.w;
			position = view_to_camera * position;
			float camera_depth = -position.z;
			if (operation == 0) {
				result = min(result, vec4(camera_depth, -camera_depth, 0.0, 0.0));
			} else if (camera_depth >= partition.x && camera_depth <= partition.y) {
				vec3 light_position = (camera_to_light * position).xyz;
				result = min(result, vec4(operation == 1 ? light_position : -light_position, 0.0));
			}
#else
			result = min(result, texelFetch(source_texture, pixel, 0));
#endif
		}
	}
	frag_color = result;
}
