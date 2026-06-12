#include <MATERIALFACTORY>
#include <VERTEXFACTORY>
#include "StandardLighting.hlsl"
#include "AmbientDistortion.hlsl"

// GLOBAL COMPILATION CONTROL
#ifndef PER_VERTEX_LUMINANCE
#define PER_VERTEX_LUMINANCE 0
#endif

#ifndef MAX_INCIDENT_LIGHTS
#define MAX_INCIDENT_LIGHTS 1
#endif

#ifndef RENDER_BASE_PASS
#define RENDER_BASE_PASS 0
#endif

#ifndef USING_COMPOSITE_LAYERS
#define USING_COMPOSITE_LAYERS 0
#endif


#define LIGHT_TABLE_SIZE (MAX_INCIDENT_LIGHTS+1)

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// VERTEX SHADER LITERALS
LITERAL(int, LuminanceMapUV_Idx1, 0);
LITERAL(int, LuminanceMapUV_Idx2, 0);

// user structs
struct VSInput
{
	VertexData vertex;
};


///////////////////////////////////////////////////////////
// PackedLightData 

//stores tightly packed light info to free up registers when passing into the pixel shader
#ifdef _XBOX
// xbox has more interpolators allowing 6 lights per pass
// note that TEXCOORD6,7 are used in PixelData and TEXCOORD0,1,2 are used in MaterialData
struct PackedLightingData
{
	row_major half3x4	normalToWorld	: TEXCOORD8;	// to transform tangent space into world space

#if USING_COMPOSITE_LAYERS
	float3	luminanceData_4to6			: TEXCOORD3;	// luminance for lights 4,5,6.  1,2,3 will be packed into the normalToWorld matrix
#endif
};

#else
// pc, only allows 3 lights per pass.
// if we have 3 lights, then we need to pack luminance data.
struct PackedLightingData
{
	row_major half3x4  normalToWorld	: TEXCOORD3;	// to transform tangent space into world space

#if USING_COMPOSITE_LAYERS
	half3	luminanceData_4to6			: COLOR0;		// luminance for lights 4,5,6.  1,2,3 will be packed into the normalToWorld matrix
#endif
};

#endif



// vertex shader constants
float4		luminanceMapUVPacking;		// .xy = scale, .zw = offset

///////////////////////////////////////////////////////////
// Layer Lighting information:
// Broken into arrays so that only the necessary elements need to be set
//	{
struct LightData
{
		float4	worldVector;
		float4	color;
		float4	channelMask;
};

LightData lights[LIGHT_TABLE_SIZE];

row_major float4x3	localToWorld;				// 3x3 local to world for caluclating normalToWorldX/Y


row_major float4x4	screenToWorld;				// 3x4 screen to world
float3		worldEyePos;


PackedLightingData PackLightingData(VSInput input)
{
	PackedLightingData pack;

	// extract indicies
	int indices[6] = {1,2,3,4,5,6};

	// calculate normal to world transform
	pack.normalToWorld[0].xyz = mul(input.vertex.tangent, localToWorld);
	pack.normalToWorld[1].xyz = mul(input.vertex.binormal, localToWorld);
	pack.normalToWorld[2].xyz = mul(input.vertex.normal, localToWorld);

	// calc luminance data
	float3 luminanceData[2] = {float3(0,0,0), float3(0,0,0)};
#if PER_VERTEX_LUMINANCE
	luminanceData[0].xyz = input.vertex.color[0].xyz;		// luminance stored in color idx 0 and 1
	luminanceData[1].xyz = input.vertex.color[1].xyz;		
	
#ifdef D3DDRV10
	// need to swap colors from bgr -> rgb
	half temp = luminanceData[0].z;
	luminanceData[0].z = luminanceData[0].x;
	luminanceData[0].x = temp;
	temp = luminanceData[1].z;
	luminanceData[1].z = luminanceData[1].x;
	luminanceData[1].x = temp;	
#endif
	
#else
	luminanceData[0].xy = input.vertex.UV[LuminanceMapUV_Idx1] * luminanceMapUVPacking.xy + luminanceMapUVPacking.zw;
	luminanceData[1].xy = input.vertex.UV[LuminanceMapUV_Idx2] * luminanceMapUVPacking.xy + luminanceMapUVPacking.zw;
#endif

	// pack luminace data
	pack.normalToWorld[0].w = luminanceData[0].x;
	pack.normalToWorld[1].w = luminanceData[0].y;
	pack.normalToWorld[2].w = luminanceData[0].z;

	// not using light indices so add some more lights
#if USING_COMPOSITE_LAYERS
	pack.luminanceData_4to6 = luminanceData[1];
#endif

	return pack;
}

// this output struct packs the data for 3 separate light sources
struct VSOutput
{
	MaterialData		material;
	PixelData			pixel;

	PackedLightingData	pack;
};

void VSMain(VSInput input, out VSOutput ret, out float4 pos : POSITION
#if DX10_CLIPPING
	,out float svclip : SV_ClipDistance
#endif
)
{	
	DEFINE_LITERALS;
	applyVertexFactory( input.vertex );

#ifdef USE_MATERIAL_MODIFY
	materialModify(input.vertex);
#endif

	pos = SI_ComputePos(input.vertex);
#if DX10_CLIPPING
	svclip = SI_ComputeClip(pos);
#endif
	ret.pixel = SI_ComputePixelData(input.vertex);
	
	ret.material = materialGenerateData(input.vertex);
	ret.pack = PackLightingData(input);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// pixel shader samplers
sampler s_luminanceMap1;
sampler s_luminanceMap2;
sampler s_shadowMask;

struct LightingData
{
	float3		worldNormal;
	float3		worldView;
	float3		worldPos;
	row_major	half3x3		normalToWorld;
	float3		lightVectors[MAX_INCIDENT_LIGHTS];
	half4		lightColors[MAX_INCIDENT_LIGHTS];	// alpha = shadow mask channel ID
	float3		luminanceData[2];						// either a tex coord or raw luminance values
};

LightingData UnpackLightingData(PackedLightingData pack, MaterialData material, PixelData pixel)
{
	LightingData data;

	half3 tangentNormal = materialNormal(material, pixel);

	data.normalToWorld[0] = pack.normalToWorld[0].xyz;
	data.normalToWorld[1] = pack.normalToWorld[1].xyz;
	data.normalToWorld[2] = pack.normalToWorld[2].xyz;
	data.worldNormal = normalize(mul(tangentNormal, data.normalToWorld));

	float4 screenPosInput = float4(pixel.screenData.xyw, 1.f);
	data.worldPos = mul(screenPosInput, screenToWorld);
	
	data.worldView = normalize(worldEyePos - data.worldPos);
	
	COMPILE_ATTRIBUTE([unroll])
	for (int i=0; i < MAX_INCIDENT_LIGHTS;i++)
	{
		// apply the same technique as modelSpaceLightVector, except done in world space
		data.lightVectors[i] = lights[i+1].worldVector.xyz - data.worldPos * lights[i+1].worldVector.w;
	}
	
	// light info
	// get light info directly from constants
	COMPILE_ATTRIBUTE([unroll])
	for (int i=0; i < MAX_INCIDENT_LIGHTS;++i)
	{
		// note: first entry of the light table is always black, so use the next one
		data.lightColors[i] = lights[i+1].color;
	}

	data.luminanceData[0].x = pack.normalToWorld[0].w;
	data.luminanceData[0].y = pack.normalToWorld[1].w;
	data.luminanceData[0].z = pack.normalToWorld[2].w;
	data.luminanceData[1] = float3(0,0,0);

#if USING_COMPOSITE_LAYERS
	data.luminanceData[1] = pack.luminanceData_4to6;
#endif

	return data;
}


// methods
half4 PSMain(VSOutput input) : COLOR
{	
	DEFINE_LITERALS;
	// unpack lighting data
	LightingData data = UnpackLightingData(input.pack, input.material, input.pixel);

	// luminace map has swizzled entries (optimised for DXT compression). unswizzle with .yzxw (see Engine\Inc\Lighting\LayerLighting.h)
#if PER_VERTEX_LUMINANCE
	half luminance[6] = {data.luminanceData[0].y, data.luminanceData[0].z, data.luminanceData[0].x, 
						data.luminanceData[1].y, data.luminanceData[1].z, data.luminanceData[1].x};
#else
	half3 luminance1 = tex2D(s_luminanceMap1, data.luminanceData[0]).yzx;
	half3 luminance2 = half3(0,0,0);	
#if USING_COMPOSITE_LAYERS
	luminance2 = tex2D(s_luminanceMap2, data.luminanceData[1]).yzx;
#endif
	half luminance[6] = {luminance1.x, luminance1.y, luminance1.z, luminance2.x, luminance2.y, luminance2.z};
#endif

	half3 view = data.worldView;
	half3 normal = data.worldNormal;
	half3 diffuse = materialDiffuse(input.material, input.pixel);

	// sample shadow mask
	half4 shadowMask = tex2Dproj(s_shadowMask, input.pixel.screenData);

	// specular stuff (HLSL will optimze this out if no specular on material)
#ifdef CHEAP_SPECULAR
	SpecularParams specularData = materialSpecularParamsCheap(input.material, input.pixel, normal);
	half3 reflectionVector = -reflect(view, specularData.normal);
#else
	SpecularParams specularData = materialSpecularParams(input.material, input.pixel, normal);
#endif

	half3 diffuseTotalColor = half3(0,0,0);
#ifndef DUAL_SPECULAR
	half3 specularTotalColor = half3(0,0,0);
#else
	half3 specularTotalColor1 = half3(0,0,0);
	half3 specularTotalColor2 = half3(0,0,0);
#endif	

	COMPILE_ATTRIBUTE([unroll])	// Predicate unrolling for the HLSL compiler
	for (int i=0; i < MAX_INCIDENT_LIGHTS; ++i)
	{
		half3 lightDir = normalize(data.lightVectors[i]);
		half shadow = saturate(dot(shadowMask, lights[i+1].channelMask));
		half3 color = saturate(luminance[i] * (1-shadow)) * data.lightColors[i].rgb;
	
		diffuseTotalColor += lightDiffuse(lightDir, normal) * color;	
#ifndef DUAL_SPECULAR

#ifdef CHEAP_SPECULAR
		specularTotalColor += lightSpecularRV(lightDir, reflectionVector, specularData.power) * color;
#else
		specularTotalColor += lightSpecularNH(lightDir, specularData.normal, view, specularData.power) * color; 
#endif
		
#else
		half2 specular = lightSpecularNH_Dual(lightDir, specularData.normal, view, specularData.powers);
		specularTotalColor1 += specular.x * color;
		specularTotalColor2 += specular.y * color;
#endif
	}

	// NOTE: HLSL BUG! If we swap the order of the specular and diffuse terms here, code does not compile properly! (no specular shows up)
#ifndef DUAL_SPECULAR
	half3 lighting = (specularData.color * specularTotalColor + diffuse * diffuseTotalColor);
#else
	half3 lighting = (specularData.color1 * specularTotalColor1 + specularData.color2 * specularTotalColor2 + diffuse * diffuseTotalColor);
#endif

#if RENDER_BASE_PASS
	// apply specular cubemap contribution
	lighting += baseSpecular(input.material, input.pixel, input.pack.normalToWorld);

	return applyBaseColor(input.material, input.pixel, input.pack.normalToWorld[2], lighting);
#else
	half4 output;
	output.rgb = lighting;
	output.a = materialAlpha(input.material, input.pixel);

	return screenTransfer(output, input.pixel);
#endif
}