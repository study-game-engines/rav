#include <ShaderTranspiler.hpp>
#include <SPIRV/GlslangToSpv.h>
#include <StandAlone/DirStackFileIncluder.h>
#include <filesystem>
#include <spirv_glsl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv_reflect.h>
#include <iostream>
#include <sstream>
#include <tint/tint.h>
#include <atomic>

#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
#define _UWP 1   
#else
#define _UWP 0
#endif

#define NEW_DXC (ST_BUNDLED_DXC || _WIN32)

#if NEW_DXC
	#include <atlcomcli.h>
#if ST_BUNDLED_DXC
	#include <dxc/dxcapi.h>
#else
	#include <dxcapi.h>
#endif
	#include <wrl/client.h>
	#include <cstdint>
	#include <locale>
	#include <codecvt>
	using namespace Microsoft::WRL;	
#endif

#ifdef _MSC_VER
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler")
#endif

using namespace std;
using namespace std::filesystem;
using namespace shadert;

static std::atomic<bool> glslAngInitialized = false;
static std::atomic<bool> tintInit = false;
static std::mutex tintInitMtx;

ReflectData::Resource::Resource(const spirv_cross::Resource& other) : id(other.id), type_id(other.type_id), base_type_id(other.base_type_id), name(std::move(other.name)){}

static ReflectData getReflectData(const spirv_cross::Compiler& comp, const spirvbytes& spirvdata){
	auto rsc = comp.get_shader_resources();
	ReflectData refl{
		.uniform_buffers{rsc.uniform_buffers.data(),rsc.uniform_buffers.begin() + rsc.uniform_buffers.size()},
		.storage_buffers{rsc.storage_buffers.data(),rsc.storage_buffers.begin() + rsc.storage_buffers.size()},
		.stage_inputs{rsc.stage_inputs.data(),rsc.stage_inputs.begin() + rsc.stage_inputs.size()},
		.stage_outputs{rsc.stage_outputs.data(),rsc.stage_outputs.begin() + rsc.stage_outputs.size()},
		.subpass_inputs{rsc.subpass_inputs.data(),rsc.subpass_inputs.begin() + rsc.subpass_inputs.size()},
		.storage_images{rsc.storage_images.data(),rsc.storage_images.begin() + rsc.storage_images.size()},
		.sampled_images{rsc.sampled_images.data(),rsc.sampled_images.begin() + rsc.sampled_images.size()},
		.atomic_counters{rsc.atomic_counters.data(),rsc.atomic_counters.begin() + rsc.atomic_counters.size()},
		.acceleration_structures{rsc.acceleration_structures.data(),rsc.acceleration_structures.begin() + rsc.acceleration_structures.size()},
		.push_constant_buffers{rsc.push_constant_buffers.data(),rsc.push_constant_buffers.begin() + rsc.push_constant_buffers.size()},
		.separate_images{rsc.separate_images.data(),rsc.separate_images.begin() + rsc.separate_images.size()},
		.separate_samplers{rsc.separate_samplers.data(),rsc.separate_samplers.begin() + rsc.separate_samplers.size()},
	};
	
	
	// since the data in stage_inputs, etc is in effectively random order
	// we need to use spirv-reflect to get the locations and order them properly
	SpvReflectShaderModule spvModule;
	SpvReflectResult result = spvReflectCreateShaderModule(spirvdata.size() * sizeof(spirvdata[0]), spirvdata.data(), &spvModule);
	if (result != SPV_REFLECT_RESULT_SUCCESS){
		throw runtime_error("SPIRV reflection capture failed");
	}
	
	const auto sortfn = [](const auto& spvreflvars, auto& inoutscontainer){
		std::unordered_map<std::string_view, uint16_t> varToPos;
		for(const auto& var : spvreflvars){
			varToPos[var->name] = var->location;
		}
		
		std::sort(inoutscontainer.begin(), inoutscontainer.end(), [&](const auto& a, const auto& b){
			return varToPos[a.name] < varToPos[b.name];
		});
	};
	
	// sort inputs
	{
		uint32_t var_count = 0;
		result = spvReflectEnumerateInputVariables(&spvModule, &var_count, NULL);
		std::vector<SpvReflectInterfaceVariable*> input_vars;
		input_vars.resize(var_count);
		result = spvReflectEnumerateInputVariables(&spvModule, &var_count, input_vars.data());
		
		sortfn(input_vars, refl.stage_inputs);
	}
	// sort outputs
	{
		uint32_t var_count = 0;
		result = spvReflectEnumerateOutputVariables(&spvModule, &var_count, NULL);
		std::vector<SpvReflectInterfaceVariable*> output_vars;
		output_vars.resize(var_count);
		result = spvReflectEnumerateOutputVariables(&spvModule, &var_count, output_vars.data());
		
		sortfn(output_vars, refl.stage_outputs);
	}
    // get compute dispatch dimensions
    for(int i = 0; i < 3; i++){
        refl.compute_dim[i] = comp.get_execution_mode_argument(spv::ExecutionMode::ExecutionModeLocalSize, i);
    }
	
	spvReflectDestroyShaderModule(&spvModule);
	
	return refl;
}


/**
* Set the name of the main function 
* @param compiler the compiler instance to use
* @param entrypoint_name the new name for the shader entrypoint function
*/
template<typename T>
static void setEntryPoint(T& compiler, const std::string& entrypoint_name) {
	auto entryPoints = compiler.get_entry_points_and_stages();
	if (!entryPoints.empty()) {
		compiler.rename_entry_point(entryPoints[0].name, entrypoint_name, entryPoints[0].execution_model);
	}
}

/**
 * Factory
 * see https://github.com/ForestCSharp/VkCppRenderer/blob/master/Src/Renderer/GLSL/ShaderCompiler.hpp for more info
 * @return instance of DefaultTBuiltInResource struct with appropriate fields set
 */
TBuiltInResource CreateDefaultTBuiltInResource(){
    return TBuiltInResource{
		.maxLights = 32,
		.maxClipPlanes = 6,
		.maxTextureUnits = 32,
		.maxTextureCoords = 32,
		.maxVertexAttribs = 64,
		.maxVertexUniformComponents = 4096,
		.maxVaryingFloats = 64,
		.maxVertexTextureImageUnits = 32,
		.maxCombinedTextureImageUnits = 80,
		.maxTextureImageUnits = 32,
		.maxFragmentUniformComponents = 4096,
		.maxDrawBuffers = 32,
		.maxVertexUniformVectors = 128,
		.maxVaryingVectors = 8,
		.maxFragmentUniformVectors = 16,
		 .maxVertexOutputVectors = 16,
		 .maxFragmentInputVectors = 15,
		 .minProgramTexelOffset = -8,
		 .maxProgramTexelOffset = 7,
		 .maxClipDistances = 8,
		 .maxComputeWorkGroupCountX = 65535,
		 .maxComputeWorkGroupCountY = 65535,
		 .maxComputeWorkGroupCountZ = 65535,
		 .maxComputeWorkGroupSizeX = 1024,
		 .maxComputeWorkGroupSizeY = 1024,
		 .maxComputeWorkGroupSizeZ = 64,
		 .maxComputeUniformComponents = 1024,
		 .maxComputeTextureImageUnits = 16,
		 .maxComputeImageUniforms = 8,
		 .maxComputeAtomicCounters = 8,
		 .maxComputeAtomicCounterBuffers = 1,
		 .maxVaryingComponents = 60,
		 .maxVertexOutputComponents = 64,
		 .maxGeometryInputComponents = 64,
		 .maxGeometryOutputComponents = 128,
		 .maxFragmentInputComponents = 128,
		 .maxImageUnits = 8,
		 .maxCombinedImageUnitsAndFragmentOutputs = 8,
		 .maxCombinedShaderOutputResources = 8,
		 .maxImageSamples = 0,
		 .maxVertexImageUniforms = 0,
		 .maxTessControlImageUniforms = 0,
		 .maxTessEvaluationImageUniforms = 0,
		 .maxGeometryImageUniforms = 0,
		 .maxFragmentImageUniforms = 8,
		 .maxCombinedImageUniforms = 8,
		 .maxGeometryTextureImageUnits = 16,
		 .maxGeometryOutputVertices = 256,
		 .maxGeometryTotalOutputComponents = 1024,
		 .maxGeometryUniformComponents = 1024,
		 .maxGeometryVaryingComponents = 64,
		 .maxTessControlInputComponents = 128,
		 .maxTessControlOutputComponents = 128,
		 .maxTessControlTextureImageUnits = 16,
		 .maxTessControlUniformComponents = 1024,
		 .maxTessControlTotalOutputComponents = 4096,
		 .maxTessEvaluationInputComponents = 128,
		 .maxTessEvaluationOutputComponents = 128,
		 .maxTessEvaluationTextureImageUnits = 16,
		 .maxTessEvaluationUniformComponents = 1024,
		 .maxTessPatchComponents = 120,
		 .maxPatchVertices = 32,
		 .maxTessGenLevel = 64,
		 .maxViewports = 16,
		 .maxVertexAtomicCounters = 0,
		 .maxTessControlAtomicCounters = 0,
		 .maxTessEvaluationAtomicCounters = 0,
		 .maxGeometryAtomicCounters = 0,
		 .maxFragmentAtomicCounters = 8,
		 .maxCombinedAtomicCounters = 8,
		 .maxAtomicCounterBindings = 1,
		 .maxVertexAtomicCounterBuffers = 0,
		 .maxTessControlAtomicCounterBuffers = 0,
		 .maxTessEvaluationAtomicCounterBuffers = 0,
		 .maxGeometryAtomicCounterBuffers = 0,
		 .maxFragmentAtomicCounterBuffers = 1,
		 .maxCombinedAtomicCounterBuffers = 1,
		 .maxAtomicCounterBufferSize = 16384,
		 .maxTransformFeedbackBuffers = 4,
		 .maxTransformFeedbackInterleavedComponents = 64,
		 .maxCullDistances = 8,
		 .maxCombinedClipAndCullDistances = 8,
		 .maxSamples = 4,
		 .limits = {
			 .nonInductiveForLoops = 1,
			 .whileLoops = 1,
			 .doWhileLoops = 1,
			 .generalUniformIndexing = 1,
			 .generalAttributeMatrixVectorIndexing = 1,
			 .generalVaryingIndexing = 1,
			 .generalSamplerIndexing = 1,
			 .generalVariableIndexing = 1,
			 .generalConstantMatrixVectorIndexing = 1,
		}
	};
}

struct CompileGLSLResult {
	spirvbytes spirvdata;
	std::vector<Uniform> uniforms;
	std::vector<LiveAttribute> attributes;
};

constexpr int textureBindingOffset = 16;

const CompileGLSLResult CompileGLSL(const std::string_view& source, const EShLanguage ShaderType, const std::vector<std::filesystem::path>& includePaths, bool debug) {
	//initialize. Do only once per process!
	if (!glslAngInitialized)
	{
		glslang::InitializeProcess();
		glslAngInitialized = true;
	}

	const char* InputCString = source.data();

	//determine the stage
	glslang::TShader shader(ShaderType);

	//set the associated strings (in this case one, but shader meta JSON can describe more. Pass as a C array and a size.
	shader.setStrings(&InputCString, 1);
	shader.setAutoMapBindings(true);
	shader.setShiftBinding(glslang::EResTexture, textureBindingOffset);
	shader.setShiftBinding(glslang::EResSampler, textureBindingOffset);
	shader.setShiftBinding(glslang::EResImage, textureBindingOffset);
    shader.setEnvInputVulkanRulesRelaxed(); // use GL_EXT_vulkan_glsl_relaxed TODO: make this configurable

	//=========== vulkan versioning (should alow this to be passed in, or find out from the system) ========
	const int DefaultVersion = 460;

	int ClientInputSemanticsVersion = DefaultVersion;
	glslang::EShTargetClientVersion VulkanClientVersion = glslang::EShTargetVulkan_1_2;
	glslang::EShTargetLanguageVersion TargetVersion = glslang::EShTargetSpv_1_3;

	shader.setEnvInput(glslang::EShSourceGlsl, ShaderType, glslang::EShClientVulkan, ClientInputSemanticsVersion);
	shader.setEnvClient(glslang::EShClientVulkan, VulkanClientVersion);
	shader.setEnvTarget(glslang::EShTargetSpv, TargetVersion);

	auto DefaultTBuiltInResource = CreateDefaultTBuiltInResource();

	TBuiltInResource Resources(DefaultTBuiltInResource);
	EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

	// =============================== preprocess GLSL =============================
	DirStackFileIncluder Includer;

	//Get Path of File
	for (const auto& path : includePaths) {
		Includer.pushExternalLocalDirectory(path.string());
	}

	std::string PreprocessedGLSL;

	if (!shader.preprocess(&Resources, DefaultVersion, ENoProfile, false, false, messages, &PreprocessedGLSL, Includer))
	{
		string msg = string("GLSL Preprocessing failed: ") + shader.getInfoLog() + "\n" + shader.getInfoDebugLog();
		throw std::runtime_error(msg);
	}

	// update the stored strings (is the original set necessary?)
	const char* PreprocessedCStr = PreprocessedGLSL.c_str();
	shader.setStrings(&PreprocessedCStr, 1);

	// ================ now parse the shader ================
	if (!shader.parse(&Resources, DefaultVersion, false, messages))
	{
		string msg = string("GLSL Parsing failed: ") + shader.getInfoLog() + "\n" + shader.getInfoDebugLog();
		throw std::runtime_error(msg);
	}

	// ============== pass parsed shader and link it ==============
	glslang::TProgram program;
	program.addShader(&shader);

	if (!program.link(messages))
	{
		std::string msg = string("GLSL Linking failed:") + program.getInfoLog() + "\n" + program.getInfoDebugLog();
		throw std::runtime_error(msg);
	}

	CompileGLSLResult result;

	// ========= convert to spir-v ============

	spv::SpvBuildLogger logger;
	glslang::SpvOptions spvOptions;
	spvOptions.generateDebugInfo = debug;
	spvOptions.disableOptimizer = debug;
	spvOptions.stripDebugInfo = !debug;
	glslang::GlslangToSpv(*program.getIntermediate(ShaderType), result.spirvdata, &logger, &spvOptions);

	// get uniform information
	program.buildReflection();
	auto nUniforms = program.getNumLiveUniformVariables();
	for (int i = 0; i < nUniforms; i++) {
		Uniform uniform;
		uniform.name = std::move(program.getUniformName(i));
		uniform.arraySize = program.getUniformArraySize(i);
		uniform.bufferOffset = program.getUniformBufferOffset(i);
		uniform.glDefineType = program.getUniformType(i);
		result.uniforms.push_back(std::move(uniform));
	}

	// get LiveAttribute data
	auto nLiveAttr = program.getNumLiveAttributes();
	for (int i = 0; i < nLiveAttr; i++) {
		auto name = program.getAttributeName(i);
		result.attributes.push_back({ name });
	}

	return result;
}

/**
 Compile GLSL to SPIR-V bytes
 @param filename the file to compile
 @param ShaderType the type of shader to compile
 */
const CompileGLSLResult CompileGLSLFromFile(const FileCompileTask& task, const EShLanguage ShaderType, bool debug){
	
	
	//Load GLSL into a string
	std::ifstream file(task.filename);
	
	if (!file.is_open())
	{
		throw std::runtime_error("failed to open file: " + task.filename.string());
	}
		
	//read input file into string, convert to C string
	std::string InputGLSL((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
	
	// add current directory
	std::vector<std::filesystem::path> pathsWithParent(std::move(task.includePaths));
	pathsWithParent.push_back(task.filename.parent_path());
	return CompileGLSL(InputGLSL, ShaderType, pathsWithParent, debug);
}

/**
 Decompile SPIR-V to OpenGL ES shader
 @param bin the SPIR-V binary to decompile
 @return OpenGL-ES source code
 */
IMResult SPIRVToOpenGL(const spirvbytes& bin, const Options& opt, spv::ExecutionModel model){
	spirv_cross::CompilerGLSL glsl(bin);
		
	//set options
	spirv_cross::CompilerGLSL::Options options;
	options.version = opt.version;
	options.es = opt.mobile;
	options.emit_uniform_buffer_as_plain_uniforms = true;
	options.vulkan_semantics = false;
	options.emit_push_constant_as_uniform_buffer = false;
	glsl.set_common_options(options);

	setEntryPoint(glsl, opt.entryPoint);

	return {glsl.compile(), "", getReflectData(glsl,bin)};
}

/**
 Decompile SPIR-V to DirectX shader
 @param bin the SPIR-V binary to decompile
 @return HLSL source code
 */
IMResult SPIRVToHLSL(const spirvbytes& bin, const Options& opt, spv::ExecutionModel model){
	spirv_cross::CompilerHLSL hlsl(bin);
	
	spirv_cross::CompilerHLSL::Options options;
	options.shader_model = opt.version;
	options.point_size_compat = true;
	hlsl.set_hlsl_options(options);

	setEntryPoint(hlsl, opt.entryPoint);

	return {hlsl.compile(), "", getReflectData(hlsl,bin)};
}

#ifdef ST_DXIL_ENABLED
IMResult SPIRVToDXIL(const spirvbytes& bin, const Options& opt, spv::ExecutionModel model){
	auto hlsl = SPIRVToHLSL(bin,opt,model);
	
#if NEW_DXC

	auto compileWithNewDxc = [&] {
		CComPtr<IDxcCompiler3> pCompiler;
		DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler));

		ComPtr<IDxcUtils> pUtils;
		DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(pUtils.GetAddressOf()));
		ComPtr<IDxcBlobEncoding> pSource;
		pUtils->CreateBlob(hlsl.sourceData.c_str(), hlsl.sourceData.size(), CP_UTF8, pSource.GetAddressOf());

		LPCWSTR profile = nullptr;
		switch (model) {
		case decltype(model)::ExecutionModelVertex:
			profile = L"vs_6_1";
			break;
		case decltype(model)::ExecutionModelFragment:
			profile = L"ps_6_1";
			break;
		case decltype(model)::ExecutionModelGLCompute:
			profile = L"cs_6_1";
			break;
		default:
			throw runtime_error("Invalid shader model");
		}

#if _UWP
		std::wstring wideEntry;
		wideEntry.resize(opt.entryPoint.size());
		mbstowcs_s(nullptr, wideEntry.data(), wideEntry.size(), opt.entryPoint.data(), wideEntry.size());
#else
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wideEntry = converter.from_bytes(opt.entryPoint);
#endif

		std::vector<LPCWSTR> arguments;
		//-E for the entry point (eg. PSMain)
		arguments.push_back(L"-E");
		arguments.push_back(wideEntry.c_str());

		//-T for the target profile (eg. ps_6_2)
		arguments.push_back(L"-T");
		arguments.push_back(profile);

		//Strip reflection data and pdbs (see later)
		//arguments.push_back(L"-Qstrip_debug");
		//arguments.push_back(L"-Qstrip_reflect");

		//arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS); //-WX
		if (opt.debug) {
			arguments.push_back(DXC_ARG_DEBUG); //-Zi
		}
		//arguments.push_back(DXC_ARG_PACK_MATRIX_ROW_MAJOR); //-Zp

		std::vector<std::wstring> defines;	// currently we have none
		for (const std::wstring& define : defines)
		{
			arguments.push_back(L"-D");
			arguments.push_back(define.c_str());
		}

		DxcBuffer sourceBuffer;
		sourceBuffer.Ptr = pSource->GetBufferPointer();
		sourceBuffer.Size = pSource->GetBufferSize();
		sourceBuffer.Encoding = 0;

		ComPtr<IDxcResult> pCompileResult;
		HRESULT result = pCompiler->Compile(&sourceBuffer, arguments.data(), arguments.size(), nullptr, IID_PPV_ARGS(pCompileResult.GetAddressOf()));

		//Error Handling
		ComPtr<IDxcBlobUtf8> pErrors;
		pCompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(pErrors.GetAddressOf()), nullptr);
		if (pErrors && pErrors->GetStringLength() > 0)
		{
			auto msg = (char*)pErrors->GetBufferPointer();
			throw runtime_error(msg);
		}

		// get the debug data (TODO: unused)
		ComPtr<IDxcBlob> pDebugData;
		ComPtr<IDxcBlobUtf16> pDebugDataPath;
		pCompileResult->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(pDebugData.GetAddressOf()), pDebugDataPath.GetAddressOf());

		// get reflection data (TODO: unused)
		ComPtr<IDxcBlob> pReflectionData;
		pCompileResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(pReflectionData.GetAddressOf()), nullptr);
		DxcBuffer reflectionBuffer;
		reflectionBuffer.Ptr = pReflectionData->GetBufferPointer();
		reflectionBuffer.Size = pReflectionData->GetBufferSize();
		reflectionBuffer.Encoding = 0;
		ComPtr<ID3D12ShaderReflection> pShaderReflection;
		pUtils->CreateReflection(&reflectionBuffer, IID_PPV_ARGS(pShaderReflection.GetAddressOf()));

		ComPtr<IDxcBlob> pShaderBinary;
		pCompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(pShaderBinary.GetAddressOf()), nullptr);
		const auto binarySize = pShaderBinary->GetBufferSize();
		hlsl.binaryData = decltype(hlsl.binaryData){(char*)pShaderBinary->GetBufferPointer(), binarySize};
	};
#endif
#if defined _MSC_VER
	auto compileWithOldDxc = [&]() {
		LPCSTR profile = nullptr;
		switch (model) {
		case decltype(model)::ExecutionModelVertex:
			profile = "vs_5_0";
			break;
		case decltype(model)::ExecutionModelFragment:
			profile = "ps_5_0";
			break;
		case decltype(model)::ExecutionModelGLCompute:
			profile = "cs_5_0";
			break;
		default:
			throw runtime_error("Invalid shader model");
		}
		ID3DBlob* code = nullptr;
		ID3DBlob* errormsg = nullptr;
		auto result = D3DCompile(
			hlsl.sourceData.data(),
			hlsl.sourceData.size(),
			"ST_HLSL.hlsl",
			nullptr,
			nullptr,
			"main",
			profile,
			D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, //TODO: make this configurable, see https://docs.microsoft.com/en-us/windows/win32/direct3dhlsl/d3dcompile-constants for flags
			0,			// see https://docs.microsoft.com/en-us/windows/win32/direct3dhlsl/d3dcompile-effect-constants for flags
			&code,
			&errormsg
		);
		if (result == S_OK) {
			hlsl.binaryData = decltype(hlsl.binaryData){(char*)code->GetBufferPointer(), code->GetBufferSize()};
		}
		else {
			throw runtime_error((char*)errormsg->GetBufferPointer());
		}
	};
	if (opt.version > 50) {
#if NEW_DXC
		compileWithNewDxc();
#else
		throw runtime_error("Cannot compile this version with legacy compiler");
#endif
	}
	else {
		compileWithOldDxc();
	}
#else
	#error DXIL is not available on this platform
#endif
	
	return hlsl;
}
#endif

/**
 Decompile SPIR-V to Metal shader
 @param bin the SPIR-V binary to decompile
 @param mobile set to True to compile for Apple Mobile platforms
 @return Metal shader source code
 */
IMResult SPIRVtoMSL(const spirvbytes& bin, const Options& opt, spv::ExecutionModel model){
	spirv_cross::CompilerMSL msl(bin);
	
	spirv_cross::CompilerMSL::Options options;
	uint32_t major = opt.version / 10;
	uint32_t minor = opt.version % 10;
	options.set_msl_version(major,minor);
	options.platform = opt.mobile ? spirv_cross::CompilerMSL::Options::Platform::iOS : spirv_cross::CompilerMSL::Options::Platform::macOS;
	options.enable_decoration_binding = true;	// order textures / samplers by binding order, not by order of first use
	msl.set_msl_options(options);
    
	auto refldata = getReflectData(msl,bin);
    
    if (opt.uniformBufferSettings.renameBuffer){
        if (refldata.uniform_buffers.size() > 0){
            auto resource = refldata.uniform_buffers[0];
            msl.set_name(resource.id, opt.uniformBufferSettings.newBufferName);
        }
    }
    
    uint8_t currentIndex = opt.pushConstantSettings.firstIndex;
    for(auto& resource : refldata.push_constant_buffers){
        spirv_cross::MSLResourceBinding newBinding;
        newBinding.stage = model;
        newBinding.desc_set = spirv_cross::ResourceBindingPushConstantDescriptorSet;
        newBinding.binding = spirv_cross::ResourceBindingPushConstantBinding;
        newBinding.msl_buffer = currentIndex;
        msl.add_msl_resource_binding( newBinding );
        
        currentIndex++;
    }
    
	setEntryPoint(msl, opt.entryPoint);
    auto res = msl.compile();
    
	return {std::move(res), "", std::move(refldata)};
}

IMResult SPIRVToWGSL(const spirvbytes& bin, const Options& opt, spv::ExecutionModel model) {
	tintInitMtx.lock();
	if (!tintInit) {
		tint::Initialize();
		tintInit = true;
	}
	tintInitMtx.unlock();

	auto tintprogram = tint::reader::spirv::Parse(bin, {
		.allow_non_uniform_derivatives = true	
	});
	if (tintprogram.Diagnostics().contains_errors()) {
		throw runtime_error(tintprogram.Diagnostics().str());
	}

	auto result = tint::writer::wgsl::Generate(&tintprogram, {});
	if (!result.success) {
		throw runtime_error(result.error);
	}

	return { result.wgsl, "", {} };
}

#if __APPLE__
extern IMResult SPIRVtoMBL(const spirvbytes& bin, const Options& opt, spv::ExecutionModel model);
#endif
/**
 Serialize a SPIR-V binary
 @param bin the binary
 */
CompileResult SerializeSPIRV(const spirvbytes& bin){
	ostringstream buffer(ios::binary);
	buffer.write((char*)&bin[0], bin.size() * sizeof(uint32_t));
	return CompileResult{{.sourceData = "", .binaryData = buffer.str()}};
}

/**
 Perform standard optimizations on a SPIR-V binary
 @param bin the SPIR-V binary to optimize
 @param options settings for the optimizer
 */
spirvbytes OptimizeSPIRV(const spirvbytes& bin, const Options &options){
	
	spv_target_env target;
	switch(options.version){
		case 10:
			target = SPV_ENV_UNIVERSAL_1_0;
			break;
		case 11:
			target = SPV_ENV_UNIVERSAL_1_1;
			break;
		case 12:
			target = SPV_ENV_UNIVERSAL_1_2;
			break;
		case 13:
			target = SPV_ENV_UNIVERSAL_1_3;
			break;
		case 14:
			target = SPV_ENV_UNIVERSAL_1_4;
			break;
		case 15:
			target = SPV_ENV_UNIVERSAL_1_5;
			break;
		default:
			throw runtime_error("Unknown Vulkan version");
			break;
	}
	
	spvtools::MessageConsumer consumer = [&](spv_message_level_t level, const char* source, const spv_position_t& position, const char* message){
		switch(level){
			case SPV_MSG_FATAL:
			case SPV_MSG_INTERNAL_ERROR:
			case SPV_MSG_ERROR:
				throw runtime_error(message);
				break;
            case SPV_MSG_WARNING:
            case SPV_MSG_INFO:
            case SPV_MSG_DEBUG:
                break;
        }
	};
    
    //create a general optimizer
	spvtools::Optimizer optimizer(target);
	optimizer.RegisterSizePasses();
	optimizer.RegisterPerformancePasses();
	optimizer.RegisterLegalizationPasses();
	optimizer.SetMessageConsumer(consumer);
	
	spirvbytes newbin;
	if (optimizer.Run(bin.data(), bin.size(), &newbin)){
		return newbin;
	}
	else{
		throw runtime_error("Failed optimizing SPIR-V binary");
	}
}

struct APIConversion {
	EShLanguage type;
	spv::ExecutionModel model;
};
static APIConversion ShaderStageToInternal(ShaderStage api) {
	APIConversion cv;

	switch (api) {
	case ShaderStage::Vertex:
		cv.type = EShLangVertex;
		cv.model = decltype(cv.model)::ExecutionModelVertex;
		break;
	case ShaderStage::Fragment:
		cv.type = EShLangFragment;
		cv.model = decltype(cv.model)::ExecutionModelFragment;
		break;
	case ShaderStage::TessControl:
		cv.type = EShLangTessControl;
		cv.model = decltype(cv.model)::ExecutionModelTessellationControl;
		break;
	case ShaderStage::TessEval:
		cv.type = EShLangTessEvaluation;
		cv.model = decltype(cv.model)::ExecutionModelTessellationEvaluation;
		break;
	case ShaderStage::Geometry:
		cv.type = EShLangGeometry;
		cv.model = decltype(cv.model)::ExecutionModelGeometry;
		break;
	case ShaderStage::Compute:
		cv.type = EShLangCompute;
		cv.model = decltype(cv.model)::ExecutionModelGLCompute;
		break;
	}

	return cv;
}

static CompileResult CompileSpirVTo(const spirvbytes& spirv, TargetAPI api, const Options& opt,  APIConversion types) {
	switch (api) {
	case TargetAPI::OpenGL:
	case TargetAPI::OpenGL_ES:
		return CompileResult{ SPIRVToOpenGL(spirv,opt,types.model) };
		break;
	case TargetAPI::Vulkan:
		if (opt.debug) {
			// don't optimize it
			return SerializeSPIRV(spirv);
		}
		else {
			return SerializeSPIRV(OptimizeSPIRV(spirv, opt));
		}
		
		break;
	case TargetAPI::HLSL:
		return CompileResult{ SPIRVToHLSL(spirv,opt,types.model) };
		break;
	case TargetAPI::Metal:
		return CompileResult{ SPIRVtoMSL(spirv,opt,types.model) };
		break;
#ifdef ST_DXIL_ENABLED
	case TargetAPI::DXIL:
		return CompileResult{ SPIRVToDXIL(spirv,opt,types.model) };
		break;
#endif
#ifdef __APPLE__
	case TargetAPI::MetalBinary:
		return CompileResult{SPIRVtoMBL(spirv,opt,types.model)};
		break;
#endif
	case TargetAPI::WGSL:
		return CompileResult{ SPIRVToWGSL(spirv,opt,types.model) };
		break;
	default:
		throw runtime_error("Unsupported API");
		break;
	}
}

CompileResult ShaderTranspiler::CompileTo(const FileCompileTask& task, TargetAPI api, const Options& opt) {

	auto types = ShaderStageToInternal(task.stage);

	//generate spirv
	auto spirv = CompileGLSLFromFile(task, types.type, opt.debug);
	auto compres = CompileSpirVTo(spirv.spirvdata, api, opt, types);
	compres.data.uniformData = std::move(spirv.uniforms);
	compres.data.attributeData = std::move(spirv.attributes);
	return compres;
}

CompileResult ShaderTranspiler::CompileTo(const MemoryCompileTask& task, TargetAPI api, const Options& opt) {
	auto types = ShaderStageToInternal(task.stage);
	auto spirv = CompileGLSL(task.source, types.type, task.includePaths, opt.debug);
	auto compres = CompileSpirVTo(spirv.spirvdata, api, opt, types);
	compres.data.uniformData = std::move(spirv.uniforms);
	compres.data.attributeData = std::move(spirv.attributes);
	return compres;
}

shadert::ShaderTranspiler::~ShaderTranspiler()
{
	
}
