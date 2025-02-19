//
//  World.cpp
//  RavEngine_Static
//
//  Copyright © 2020 Ravbug.
//

#include "World.hpp"
#include <iostream>
#include <algorithm>
#include "ScriptComponent.hpp"
#include "App.hpp"
#include "PhysicsLinkSystem.hpp"
#include "GUI.hpp"
#include "InputManager.hpp"
#include "AudioRoomSyncSystem.hpp"
#include "CameraComponent.hpp"
#include "StaticMesh.hpp"
#include "BuiltinMaterials.hpp"
#include "NetworkIdentity.hpp"
#include "RPCSystem.hpp"
#include "AnimatorSystem.hpp"
#include "SkinnedMeshComponent.hpp"
#include "NetworkManager.hpp"
#include "Constraint.hpp"
#include <physfs.h>
#include "ScriptSystem.hpp"
#include "RenderEngine.hpp"
#include "Skybox.hpp"
#include "PhysicsSolver.hpp"
#include "VRAMSparseSet.hpp"
#include "PhysicsBodyComponent.hpp"

using namespace std;
using namespace RavEngine;

template<typename T>
static const World::EntitySparseSet<T> staticEmptyContainer;

template<typename T>
static inline void SetEmpty(typename World::EntitySparseSet<T>::const_iterator& begin, typename World::EntitySparseSet<T>::const_iterator& end){
    begin = staticEmptyContainer<T>.begin();
    end = staticEmptyContainer<T>.end();
}

void RavEngine::World::Tick(float scale) {
	
    PreTick(scale);
	
	//Tick the game code
	TickECS(scale);

    PostTick(scale);
}


RavEngine::World::World() : Solver(std::make_unique<PhysicsSolver>()){
    // init render data if the render engine is online
    if (GetApp() && GetApp()->HasRenderEngine() && GetApp()->GetRenderEngine().GetDevice()) {
        renderData.emplace();
    }

    SetupTaskGraph();
    EmplacePolymorphicSystem<ScriptSystem>();
    EmplaceSystem<AnimatorSystem>();
	EmplaceSystem<SocketSystem>();
    CreateDependency<AnimatorSystem,ScriptSystem>();			// run scripts before animations
    CreateDependency<AnimatorSystem,PhysicsLinkSystemRead>();	// run physics reads before animator
    CreateDependency<PhysicsLinkSystemWrite,ScriptSystem>();	// run physics write before scripts
	CreateDependency<SocketSystem, AnimatorSystem>();			// run animator before socket system
        
    EmplaceSystem<AudioRoomSyncSystem>();
    EmplaceSystem<RPCSystem>();
    if (PHYSFS_isInit()){
        skybox = make_shared<Skybox>();
    }
}

void World::NetworkingSpawn(ctti_t id, Entity& handle){
    // are we networked, and the server?
    if (NetworkManager::IsNetworked() && NetworkManager::IsServer()){
        // is the constructed type a network object?
        if(GetApp()->networkManager.isNetworkEntity(id)){
            //add a network identity to this entity
            auto& netidcomp = handle.EmplaceComponent<NetworkIdentity>(id);
            
            // now send the message to spawn this on the other end
            GetApp()->networkManager.Spawn(this,id,handle.id,netidcomp.GetNetworkID());
        }
    }
}

void World::NetworkingDestroy(entity_t id){
    Entity handle{id};
    // are we networked, and is this the server?
    if (NetworkManager::IsNetworked() && NetworkManager::IsServer()){
        // is this a networkobject?
        if(handle.HasComponent<NetworkIdentity>()){
            auto& netidcomp = handle.GetComponent<NetworkIdentity>();
            GetApp()->networkManager.Destroy(netidcomp.GetNetworkID());
        }
    }
}

/**
 Tick all of the objects in the world, multithreaded
 @param fpsScale the scale factor to apply to all operations based on the frame rate
 */
void RavEngine::World::TickECS(float fpsScale) {
	currentFPSScale = fpsScale;

	//update time
	time_now = e_clock_t::now();
	
	//execute and wait
    GetApp()->executor.run(masterTasks).wait();
	if (isRendering){
		newFrame = true;
	}
}

bool RavEngine::World::InitPhysics() {
	if (physicsActive){
		return false;
	}
	
	physicsActive = true;

	return true;
}

void World::SetupTaskGraph(){
    masterTasks.name("RavEngine Master Tasks");
	
    //TODO: FIX (use conditional tasking here)
    setupRenderTasks();
    
    ECSTasks.name("ECS");
    ECSTaskModule = masterTasks.composed_of(ECSTasks).name("ECS");
    
    // ensure Systems run before rendering
    renderTaskModule.succeed(ECSTaskModule);
    
    // process any dispatched coroutines
    auto updateAsyncIterators = ECSTasks.emplace([&]{
        async_begin = async_tasks.begin();
        async_end = async_tasks.end();
    }).name("async iterator update");
    auto doAsync = ECSTasks.for_each(std::ref(async_begin), std::ref(async_end), [&](const shared_ptr<dispatched_func>& item){
        if (GetApp()->GetCurrentTime() >= item->runAtTime){
            item->func();
            ranFunctions.push_back(async_tasks.hash_for(item));
        }
    }).name("Exec Async");
    updateAsyncIterators.precede(doAsync);
    auto cleanupRanAsync = ECSTasks.emplace([&]{
        // remove functions that have been run
        for(const auto hash : ranFunctions){
            async_tasks.erase_by_hash(hash);
        }
        ranFunctions.clear();
    }).name("Async cleanup");
    doAsync.precede(cleanupRanAsync);
    
    //add the PhysX tick, must run after write but before read

	auto physicsRootTask = ECSTasks.emplace([] {}).name("PhysicsRootTask");

	auto RunPhysics = ECSTasks.emplace([this]{
		Solver->Tick(GetCurrentFPSScale());
	}).name("PhysX Execute");
    
    auto read = EmplaceSystem<PhysicsLinkSystemRead>();
    auto write = EmplaceSystem<PhysicsLinkSystemWrite>();
    RunPhysics.precede(read.second);
    RunPhysics.succeed(write.second);
	
    physicsRootTask.precede(read.first,write.first);
	read.second.succeed(RunPhysics);	// if checkRunPhysics returns a 1, it goes here anyways.
    
    // setup audio tasks
    audioTasks.name("Audio");
    
    auto audioClear = audioTasks.emplace([this]{
        GetApp()->GetCurrentAudioSnapshot()->Clear();
        //TODO: currently this selects the LAST listener, but there is no need for this
        Filter([](const AudioListener& listener, const Transform& transform){
            auto ptr = GetApp()->GetCurrentAudioSnapshot();
            ptr->listenerPos = transform.GetWorldPosition();
            ptr->listenerRot = transform.GetWorldRotation();
            ptr->listenerGraph = listener.GetGraph();
        });
    }).name("Clear + Listener");
    
  
    
    auto copyAudios = audioTasks.emplace([this]{
        Filter([this](AudioSourceComponent& audioSource, const Transform& transform){
            GetApp()->GetCurrentAudioSnapshot()->sources.emplace(audioSource.GetPlayer(),transform.GetWorldPosition(),transform.GetWorldRotation());
        });
        
        // now clean up the fire-and-forget audios that have completed
        instantaneousToPlay.remove_if([](const InstantaneousAudioSource& ias){
            return ! ias.GetPlayer()->IsPlaying();
        });
        
        // now do fire-and-forget audios that need to play
        for(auto& f : instantaneousToPlay){
            GetApp()->GetCurrentAudioSnapshot()->sources.emplace(f.GetPlayer(),f.source_position,quaternion(0,0,0,1));
        }
    }).name("Point Audios").succeed(audioClear);
    
    auto copyAmbients = audioTasks.emplace([this]{
        // raster audio
        Filter([this](AmbientAudioSourceComponent& audioSource){
            GetApp()->GetCurrentAudioSnapshot()->ambientSources.emplace(audioSource.GetPlayer());
        });

        // now clean up the fire-and-forget audios that have completed
        ambientToPlay.remove_if([](const InstantaneousAmbientAudioSource& ias) {
            return !ias.GetPlayer()->IsPlaying();
        });
        
        // now do fire-and-forget audios that need to play
        for(auto& f : ambientToPlay){
            GetApp()->GetCurrentAudioSnapshot()->ambientSources.emplace(f.GetPlayer());
        }
        
    }).name("Ambient Audios").succeed(audioClear);
    
    auto copyRooms = audioTasks.emplace([this]{
        Filter( [this](AudioRoom& room, Transform& transform){
            GetApp()->GetCurrentAudioSnapshot()->rooms.emplace_back(room.data,transform.GetWorldPosition(),transform.GetWorldRotation());
        });
        
    }).name("Rooms").succeed(audioClear);
    
    auto audioSwap = audioTasks.emplace([]{
        GetApp()->SwapCurrrentAudioSnapshot();
    }).name("Swap Current").succeed(copyAudios,copyAmbients,copyRooms);
    
    audioTaskModule = masterTasks.composed_of(audioTasks).name("Audio");
    audioTaskModule.succeed(ECSTaskModule);
}

void World::setupRenderTasks(){
	//render engine data collector
	//camera matrices
    renderTasks.name("Render");
   
    auto resizeBuffer = renderTasks.emplace([this]{
        // can the world transform list hold that many objects?
        // to avoid an indirection, we assume all entities may have a transform
        // this wastes some VRAM
        auto nEntities = localToGlobal.size() + std::min(nCreatedThisTick, 1);  // hack: if I don't add 1, then the pbr.vsh shader OOBs, not sure why
        auto currentBufferSize = renderData->worldTransforms.size();
        if (nEntities > currentBufferSize){
            auto newSize = closest_power_of(nEntities, 16);
            renderData->worldTransforms.resize(newSize);
        }
        nCreatedThisTick = 0;
    });

    auto updateRenderDataStaticMesh = renderTasks.emplace([this] {
        
        Filter([&](const StaticMesh& sm, Transform& trns) {
            if (trns.isTickDirty && sm.GetEnabled()) {
                // update
                assert(renderData->staticMeshRenderData.contains(sm.GetMaterial()));
                auto meshToUpdate = sm.GetMesh();
                renderData->staticMeshRenderData.if_contains(sm.GetMaterial(), [&meshToUpdate,&trns,this](MDIICommand& row) {
                    auto it = std::find_if(row.commands.begin(), row.commands.end(), [&](const auto& value) {
                        return value.mesh.lock() == meshToUpdate;
                    });
                    assert(it != row.commands.end());
                    auto& vec = *it;
                    // write new matrix
                    auto owner = trns.GetOwner();
                    auto ownerIDInWorld = owner.GetIdInWorld();
                    renderData->worldTransforms[ownerIDInWorld] = trns.CalculateWorldMatrix();
                });

                trns.ClearTickDirty();
            }
        });
    }).name("Update invalidated static mesh transforms");

    auto updateRenderDataSkinnedMesh = renderTasks.emplace([this] {
        Filter([&](const SkinnedMeshComponent& sm, const AnimatorComponent& am, Transform& trns) {
            if (trns.isTickDirty && sm.GetEnabled()) {
                // update
                auto owner = trns.GetOwner();

                assert(renderData->skinnedMeshRenderData.contains(sm.GetMaterial()));
                auto meshToUpdate = sm.GetMesh();
                auto skeletonToUpdate = sm.GetSkeleton();
                renderData->skinnedMeshRenderData.if_contains(sm.GetMaterial(), [owner, &meshToUpdate, &trns, &skeletonToUpdate, this](MDIICommandSkinned& row) {
                    auto it = std::find_if(row.commands.begin(), row.commands.end(), [&](const auto& value) {
                        return value.mesh.lock() == meshToUpdate && value.skeleton.lock() == skeletonToUpdate;
                    });
                    assert(it != row.commands.end());
                    auto& vec = *it;
                    // write new matrix
                    auto ownerIDInWorld = owner.GetIdInWorld();
                    renderData->worldTransforms[ownerIDInWorld] = trns.CalculateWorldMatrix();
				});
                trns.ClearTickDirty();
            }
        });
    }).name("Upate invalidated skinned mesh transforms");
    
    resizeBuffer.precede(updateRenderDataStaticMesh, updateRenderDataSkinnedMesh);
    
    auto updateInvalidatedDirs = renderTasks.emplace([this]{
        if (auto ptr = GetAllComponentsOfType<DirectionalLight>()){
            for(int i = 0; i < ptr->DenseSize(); i++){
                auto owner = Entity(localToGlobal[ptr->GetOwner(i)]);
                auto& transform = owner.GetTransform();
                if (transform.isTickDirty){
                    // update transform data if it has changed
                    auto rot = owner.GetTransform().WorldUp();

                    // use local ID here, no need for local-to-global translation
                    renderData->directionalLightData.GetForSparseIndex(ptr->GetOwner(i)).direction = rot;
                }
                if (ptr->Get(i).isInvalidated()){
                    // update color data if it has changed
                    auto& lightdata = ptr->Get(i);
                    auto& color = lightdata.GetColorRGBA();
                    renderData->directionalLightData.GetForSparseIndex(ptr->GetOwner(i)).colorIntensity = {color.R, color.G, color.B, lightdata.GetIntensity()};
                    ptr->Get(i).clearInvalidate();
                }
                // don't reset transform tickInvalidated here because the meshUpdater needs it after this
            }
        }
    }).name("Update Invalidated DirLights").precede(updateRenderDataStaticMesh, updateRenderDataSkinnedMesh);
    
    auto updateInvalidatedSpots = renderTasks.emplace([this]{
        if (auto ptr = GetAllComponentsOfType<SpotLight>()){
            for(int i = 0; i < ptr->DenseSize(); i++){
                auto owner = Entity(localToGlobal[ptr->GetOwner(i)]);
                auto& transform = owner.GetTransform();
                if (transform.isTickDirty){
                    // update transform data if it has changed
                    renderData->spotLightData.GetForSparseIndex(ptr->GetOwner(i)).worldTransform = transform.CalculateWorldMatrix();
                }
                if (ptr->Get(i).isInvalidated()){
                    // update color data if it has changed
                    auto& lightData = ptr->Get(i);
                    auto& colorData = lightData.GetColorRGBA();
                    auto& denseData = renderData->spotLightData.GetForSparseIndex(ptr->GetOwner(i));
                    denseData.coneAndPenumbra = { lightData.GetConeAngle(), lightData.GetPenumbraAngle() };
                    denseData.colorIntensity = { colorData.R,colorData.G,colorData.B,lightData.GetIntensity()};
                    ptr->Get(i).clearInvalidate();
                }
                // don't reset transform tickInvalidated here because the meshUpdater needs it after this
            }
        }
    }).name("Update Invalidated SpotLights").precede(updateRenderDataStaticMesh, updateRenderDataSkinnedMesh);
    
    auto updateInvalidatedPoints = renderTasks.emplace([this]{
        if (auto ptr = GetAllComponentsOfType<PointLight>()){
            for(int i = 0; i < ptr->DenseSize(); i++){
                auto owner = Entity(localToGlobal[ptr->GetOwner(i)]);
                auto& transform = owner.GetTransform();
                if (transform.isTickDirty){
                    // update transform data if it has changed
                    renderData->pointLightData.GetForSparseIndex(ptr->GetOwner(i)).worldTransform = transform.CalculateWorldMatrix();
                }
                if (ptr->Get(i).isInvalidated()){
                    // update color data if it has changed
                    auto& lightData = ptr->Get(i);
                    auto& colorData = lightData.GetColorRGBA();
                    renderData->pointLightData.GetForSparseIndex(ptr->GetOwner(i)).colorIntensity = { colorData.R,colorData.G,colorData.B,lightData.GetIntensity()};
                    ptr->Get(i).clearInvalidate();
                }
                // don't reset transform tickInvalidated here because the meshUpdater needs it after this
            }
        }
    }).name("Update Invalidated SpotLights").precede(updateRenderDataStaticMesh, updateRenderDataSkinnedMesh);
    
    auto updateInvalidatedAmbients = renderTasks.emplace([this]{
        if(auto ptr = GetAllComponentsOfType<AmbientLight>()){
            for(int i = 0; i < ptr->DenseSize(); i++){
                auto ownerLocalId = ptr->GetOwner(i);
                auto& light = ptr->Get(i);
                auto& color = light.GetColorRGBA();
                renderData->ambientLightData.GetForSparseIndex(ownerLocalId) = {color.R, color.G, color.B, light.GetIntensity()};
                light.clearInvalidate();
            }
        }
    }).name("Update Invalidated AmbLights");

	auto tickGUI = renderTasks.emplace([this]() {
        auto& renderer = GetApp()->GetRenderEngine();
        auto size = renderer.GetBufferSize();
        auto scale = renderer.GetDPIScale();
        Filter([&](GUIComponent& gui) {
            if (gui.Mode == GUIComponent::RenderMode::Screenspace) {
                gui.SetDimensions(size.width, size.height);
                gui.SetDPIScale(scale);
            }
            gui.Update();
        });
	}).name("UpdateGUI");
    
    // attatch the renderTasks module to the masterTasks
    renderTaskModule = masterTasks.composed_of(renderTasks).name("Render");
}

void World::DispatchAsync(const Function<void ()>& func, double delaySeconds){
    auto time = GetApp()->GetCurrentTime();
    GetApp()->DispatchMainThread([=]{
        async_tasks.insert(make_shared<dispatched_func>(time + delaySeconds,func));
    });
}

void RavEngine::World::updateStaticMeshMaterial(entity_t localId, decltype(RenderData::staticMeshRenderData)::key_type oldMat, decltype(RenderData::staticMeshRenderData)::key_type newMat, Ref<MeshAsset> mesh)
{
    // do nothing if renderer is not online
    if (!renderData) {
        return;
    }

    // detect the case of the material set to itself
    if (oldMat == newMat) {
        return;
    }

    // if the material has changed, need to reset the old one
    if (oldMat != nullptr) {
        renderData->staticMeshRenderData.if_contains(oldMat, [&](decltype(RenderData::staticMeshRenderData)::mapped_type& value) {
            // find the Mesh
            for (auto it = value.commands.begin(); it != value.commands.end(); ++it) {
                auto& command = *it;
                auto cmpMesh = command.mesh.lock();
                if (cmpMesh == mesh) {
                    command.entities.EraseAtSparseIndex(localId);
                    if (command.entities.DenseSize() == 0) {
                        value.commands.erase(it);
                    }
                    break;
                }
            }
        });
    }

    // add the new mesh & its transform to the hashmap 
    assert(HasComponent<Transform>(localId) && "Cannot change material on an entity that does not have a transform!");
    auto& set = ( * (renderData->staticMeshRenderData.try_emplace(newMat, decltype(RenderData::staticMeshRenderData)::mapped_type()).first)).second;
    bool found = false;
    for (auto& command : set.commands) {
        auto cmpMesh = command.mesh.lock();
        if (cmpMesh == mesh) {
            found = true;
            command.entities.Emplace(localId,localId);
        }
    }
    // otherwise create a new entry
    if (!found) {
        set.commands.emplace(mesh, localId, localId);
    }
}

void RavEngine::World::updateSkinnedMeshMaterial(entity_t localId, decltype(RenderData::skinnedMeshRenderData)::key_type oldMat, decltype(RenderData::skinnedMeshRenderData)::key_type newMat, Ref<MeshAssetSkinned> mesh, Ref<SkeletonAsset> skeleton)
{
    // if render engine is not online, do nothing
    if (!renderData) {
        return;
    }

    // detect the case of the material set to itself
    if (oldMat == newMat) {
        return;
    }

    // if the material has changed, need to reset the old one
    if (oldMat != nullptr) {
        renderData->skinnedMeshRenderData.if_contains(oldMat, [&](decltype(RenderData::skinnedMeshRenderData)::mapped_type& value) {
            // find the Mesh
            for (auto it = value.commands.begin(); it != value.commands.end(); ++it) {
                auto& command = *it;
                auto cmpMesh = command.mesh.lock();
                auto cmpSkeleton = command.skeleton.lock();
                if (cmpMesh == mesh && cmpSkeleton == skeleton) {
                    command.entities.EraseAtSparseIndex(localId);
                    if (command.entities.DenseSize() == 0) {
                        value.commands.erase(it);
                    }
                    break;
                }
            }
        });
    }

    // add the new mesh, its skeleton, & its transform to the hashmap entry
    assert(HasComponent<Transform>(localId) && "Cannot change material on an entity that does not have a transform!");
    auto& transform = GetComponent<Transform>(localId);
    auto& set = (*(renderData->skinnedMeshRenderData.try_emplace(newMat, decltype(RenderData::skinnedMeshRenderData)::mapped_type()).first)).second;
    bool found = false;
    for (auto& command : set.commands) {
        auto cmpMesh = command.mesh.lock();
        auto cmpSkeleton = command.skeleton.lock();
        if (cmpMesh == mesh && cmpSkeleton == skeleton) {
            found = true;
            command.entities.Emplace(localId, localId);
        }
    }
    // otherwise create a new entry
    if (!found) {
        set.commands.emplace(mesh, skeleton, localId, localId);
    }
}

void RavEngine::World::DestroyStaticMeshRenderData(const StaticMesh& mesh, entity_t local_id)
{
    if (!renderData) {
        return;
    }

    renderData->staticMeshRenderData.modify_if(mesh.GetMaterial(), [local_id,&mesh](decltype(RenderData::staticMeshRenderData)::mapped_type& data) {
        auto it = std::find_if(data.commands.begin(), data.commands.end(), [&](auto& other) {
            return other.mesh.lock() == mesh.GetMesh();
        });
        if (it != data.commands.end() && (*it).entities.HasForSparseIndex(local_id)) {
            (*it).entities.EraseAtSparseIndex(local_id);
            // if empty, remove from the larger container
            if ((*it).entities.DenseSize() == 0) {
                data.commands.erase(it);
            }
        }
    });
}

void World::DestroySkinnedMeshRenderData(const SkinnedMeshComponent& mesh, entity_t local_id) {
    if (!renderData) {
        return;
    }

    renderData->skinnedMeshRenderData.modify_if(mesh.GetMaterial(), [local_id,&mesh](decltype(RenderData::skinnedMeshRenderData)::mapped_type& data) {
        auto it = std::find_if(data.commands.begin(), data.commands.end(), [&](auto& other) {
            return other.mesh.lock() == mesh.GetMesh() && other.skeleton.lock() == mesh.GetSkeleton();
        });
        if (it != data.commands.end() && (*it).entities.HasForSparseIndex(local_id)) {
            (*it).entities.EraseAtSparseIndex(local_id);
            // if empty, remove from the larger container
            if ((*it).entities.DenseSize() == 0) {
                data.commands.erase(it);
            }
        }
    });
}

void World::StaticMeshChangedVisibility(const StaticMesh* mesh){
	auto owner = mesh->GetOwner();
	if (mesh->GetEnabled()){
		updateStaticMeshMaterial(owner.GetIdInWorld(),nullptr,mesh->GetMaterial(),mesh->GetMesh());
	}
	else{
		DestroyStaticMeshRenderData(*mesh, owner.GetIdInWorld());
	}
}

void World::SkinnedMeshChangedVisibility(const SkinnedMeshComponent* mesh){
	auto owner = mesh->GetOwner();
	if (mesh->GetEnabled()){
		updateSkinnedMeshMaterial(owner.GetIdInWorld(),nullptr,mesh->GetMaterial(),mesh->GetMesh(),mesh->GetSkeleton());
	}
	else{
		DestroySkinnedMeshRenderData(*mesh, owner.GetIdInWorld());
	}
}


entity_t World::CreateEntity(){
    entity_t id;
    if (available.size() > 0){
        id = available.front();
        available.pop();
    }
    else{
        id = static_cast<decltype(id)>(localToGlobal.size());
        localToGlobal.push_back(INVALID_ENTITY);
        nCreatedThisTick++;
    }
    localToGlobal[id] = Registry::CreateEntity(this, id);
    return localToGlobal[id];
}

World::~World() {
    for(entity_t i = 0; i < localToGlobal.size(); i++){
        if (EntityIsValid(localToGlobal[i])){
            DestroyEntity(i); // destroy takes a local ID
        }
    }
}

void RavEngine::World::PlaySound(const InstantaneousAudioSource& ias) {
    instantaneousToPlay.push_back(ias);
}

void RavEngine::World::PlayAmbientSound(const InstantaneousAmbientAudioSource& iaas) {
    ambientToPlay.push_back(iaas);
}

void World::DeallocatePhysics(){
    Solver->DeallocatePhysx();
}

