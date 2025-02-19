
layout(location = 0) in vec4 colorintensity;
layout(location = 0) out vec4 outcolor;

layout(push_constant) uniform UniformBufferObject{
	 mat4 viewProj;
	ivec4 viewRect;
} ubo;

uniform sampler2D s_albedo;

layout(early_fragment_tests) in;
void main()
{
	//calculate sampling positions using fragment pos and view dimensions
	vec2 texcoord = vec2(gl_FragCoord.x / ubo.viewRect[2], gl_FragCoord.y / ubo.viewRect[3]);
	
	vec4 lightColor = colorintensity;
	
	float intensity = lightColor[3];
	
	vec3 albedo = texture(s_albedo, texcoord).xyz;
	
	outcolor = vec4(albedo * intensity * lightColor.xyz, 1);
}
