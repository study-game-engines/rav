#include "MeshAssetSkinned.hpp"
#include <fmt/format.h>
#include "App.hpp"
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include "Filesystem.hpp"
#include "VirtualFileSystem.hpp"
#include "RenderEngine.hpp"
#include <RGL/Device.hpp>
#include <RGL/Buffer.hpp>
#include "SkeletonAsset.hpp"

using namespace RavEngine;
using namespace std;

RavEngine::MeshAssetSkinned::~MeshAssetSkinned()
{
	GetApp()->GetRenderEngine().gcBuffers.enqueue(weightsBuffer);
}

//TODO: avoid opening the file twice -- this is a double copy and repeats work, therefore slow
MeshAssetSkinned::MeshAssetSkinned(const std::string& path, Ref<SkeletonAsset> skeleton, float scale) : MeshAsset(path,MeshAssetOptions{false,true,scale}){
	
	auto fullpath = StrFormat("objects/{}",path);
	
	if (!GetApp()->GetResources().Exists(fullpath.c_str())){
		Debug::Fatal("No asset at {}",fullpath);
	}
	
	auto str = GetApp()->GetResources().FileContentsAt(fullpath.c_str());
	
	//pull from cmrc
	auto file_ext = Filesystem::Path(fullpath).extension();
	//uses a meta-flag to auto-triangulate the input file
	const aiScene* scene = aiImportFileFromMemory(reinterpret_cast<char*>(str.data()), Debug::AssertSize<unsigned int>(str.size()),
												  aiProcess_CalcTangentSpace |
												  aiProcess_GenSmoothNormals              |
												  aiProcess_JoinIdenticalVertices         |
												  aiProcess_ImproveCacheLocality          |
												  aiProcess_LimitBoneWeights              |
												  aiProcess_RemoveRedundantMaterials      |
												  aiProcess_SplitLargeMeshes              |
												  aiProcess_Triangulate                   |
												  aiProcess_GenUVCoords                   |
												  aiProcess_SortByPType                   |
												  aiProcess_FindInstances                 |
												  aiProcess_ValidateDataStructure         |
												  aiProcess_OptimizeMeshes				  |
												  aiProcess_FindInvalidData     ,
												  file_ext.string().c_str());
	
	
	if (!scene){
		Debug::Fatal("Cannot load: {}", aiGetErrorString());
	}
	RavEngine::Vector<vweights> allweights;
	{
		uint32_t numverts = 0;
		for(int i = 0; i < scene->mNumMeshes; i++){
			numverts += scene->mMeshes[i]->mNumVertices;
		}
		
		allweights.resize(numverts);
	}
	
	uint16_t current_offset = 0;
	auto sk = skeleton->GetSkeleton().get();
	
	auto calcMesh = [&](const aiMesh* mesh){
		for(int i = 0; i < mesh->mNumBones; i++){
			auto bone = mesh->mBones[i];
			vweights weights;
			//find this bone in the skeleton to determine joint index
			auto it = std::find(sk->joint_names().begin(),sk->joint_names().end(),string(bone->mName.C_Str()));
			if (it == sk->joint_names().end()){
				continue;
			}
			uint32_t idx = std::distance(sk->joint_names().begin(), it);
			
			//copy (index + current_offset) and influence
			for(int j = 0; j < bone->mNumWeights; j++){
				auto weightval = bone->mWeights[j];
				
				allweights[weightval.mVertexId + current_offset].weights.push_back({vweights::vw{idx,weightval.mWeight}});
			}
			
		}
	};
	
	//go through mesh and pull out weights
	for(int i = 0; i < scene->mNumMeshes; i++){
		auto mesh = scene->mMeshes[i];
		
		calcMesh(mesh);
		current_offset += mesh->mNumVertices;
	}
	struct wrapper{
		vweights::vw w[4];
	};
	//make gpu version
    std::vector<wrapper> weightsgpu;
	weightsgpu.reserve(allweights.size());
	std::memset(weightsgpu.data(), 0, weightsgpu.size() * sizeof(weightsgpu[0]));
	
	for(const auto& weights : allweights){
		wrapper w;
		uint8_t i = 0;
		for(const auto& weight : weights.weights){
			w.w[i].influence = weight.influence;
			w.w[i].joint_idx = weight.joint_idx;
			
			i++;
		}
		weightsgpu.push_back(w);
	}
	
	//map to GPU
	//TODO: make buffer Private
	weightsBuffer = GetApp()->GetRenderEngine().GetDevice()->CreateBuffer({
		uint32_t(weightsgpu.size()),
		{.StorageBuffer = true},
		sizeof(wrapper),
		RGL::BufferAccess::Private,
		{.Writable = false}
	});
	weightsBuffer->SetBufferData({ weightsgpu.data(),weightsgpu.size() * sizeof(decltype(weightsgpu)::value_type)});
}

