uniform float4x4 ViewProj;
uniform texture2d image;

uniform float elapsed_time;
uniform float2 uv_offset;
uniform float2 uv_scale;
uniform float2 uv_pixel_interval;

sampler_state textureSampler {
	Filter    = Linear;
	AddressU  = Border;
	AddressV  = Border;
	BorderColor = 00000000;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData mainTransform(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv = v_in.uv * uv_scale + uv_offset;
	return vert_out;
}

float4 grayscale(float4 color)
{
	float grayscale = dot(color.rgb, float3(0.3, 0.59, 0.11));
	return float4(grayscale, grayscale, grayscale, color.a);
}

float4 gaussian(VertData v_in, float angle)
{
		float rad = radians(angle);
		float2 dir = float2(sin(rad), cos(rad)) * uv_pixel_interval.xy;
		float2 dir_2 = dir * 2.0;
		float4 ret = image.Sample(textureSampler, v_in.uv) * 0.375;
		
		float4 px_away = image.Sample(textureSampler, v_in.uv + dir);
		px_away += image.Sample(textureSampler, v_in.uv - dir);
		px_away *= 0.25;
		
		float4 px_2_away = image.Sample(textureSampler, v_in.uv + dir_2);
		px_2_away += image.Sample(textureSampler, v_in.uv + dir_2);
		px_2_away *= 0.0625;
		
		return ret + px_away + px_2_away;
}

uniform float4 text_color;
uniform float max_distance;
uniform float exp;

#define PI 3.141592653589793238462643383279502884197169399375105820974

float4 blurImageH(VertData v_in) : TARGET
{
	return gaussian(v_in, 0);
}

float4 blurImageV(VertData v_in) : TARGET
{
	return gaussian(v_in, 90);
}

float4 mainImage(VertData v_in) : TARGET
{
	float4 color = image.Sample(textureSampler, v_in.uv);
	float4 gray = grayscale(color);
	float4 gray_text = grayscale(text_color);
	float d = distance(gray.rgb, gray_text.rgb);
	float d_c = pow(d*2, exp) / pow(2, exp);
	d_c = sin(d_c * PI / 2);
	//d = pow(1.0 - sin(d * PI/4), exp);

	color.rgb = d_c;
	return color;
}

technique Draw
{
	pass b0
	{
		vertex_shader = mainTransform(v_in);
		pixel_shader = blurImageH(v_in);
	}
	
	pass b1
	{
		vertex_shader = mainTransform(v_in);
		pixel_shader = blurImageV(v_in);
	}
	
	pass p0
	{
		vertex_shader = mainTransform(v_in);
		pixel_shader = mainImage(v_in);
	}
}