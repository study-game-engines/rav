#include "AudioRoom.hpp"
#include "Entity.hpp"
#include "AudioSource.hpp"
#include "DataStructures.hpp"
#include "Transform.hpp"
#include "AudioPlayer.hpp"

#include "mathtypes.hpp"
#include <common/room_effects_utils.h>

using namespace RavEngine;
using namespace std;

AudioRoom::RoomData::RoomData() : audioEngine(vraudio::CreateResonanceAudioApi(AudioPlayer::GetNChannels(), AudioPlayer::GetBufferSize(), AudioPlayer::GetSamplesPerSec())){}

void AudioRoom::RoomData::SetListenerTransform(const vector3 &worldpos, const quaternion &wr){
	audioEngine->SetHeadPosition(worldpos.x, worldpos.y, worldpos.z);
	audioEngine->SetHeadRotation(wr.x, wr.y, wr.z, wr.w);
}

void AudioRoom::RoomData::AddEmitter(const float* data, const vector3 &pos, const quaternion &rot, const vector3 &roompos, const quaternion &roomrot, size_t code, float volume){
    
    auto& worldpos = pos;
    auto& worldrot = rot;
    
    //create Eigen structures to calculate attenuation
    vraudio::WorldPosition eworldpos(worldpos.x,worldpos.y,worldpos.z);
    vraudio::WorldRotation eroomrot(roomrot.w,roomrot.x,roomrot.y,roomrot.z);
    vraudio::WorldPosition eroompos(roompos.x,roompos.y,roompos.z);
    vraudio::WorldPosition eroomdim(roomDimensions.x,roomDimensions.y,roomDimensions.z);
    auto gain = vraudio::ComputeRoomEffectsGain(eworldpos, eroompos, eroomrot, eroomdim);
            
    // TODO: reuse audio sources across computations
    // get the audio source for the room for this source
    // if one does not exist, create it
    vraudio::ResonanceAudioApi::SourceId src;
    if (!allSources.contains(code)){
        src = audioEngine->CreateSoundObjectSource(vraudio::RenderingMode::kBinauralLowQuality);
        allSources[code] = src;
    }
    else{
        src = allSources[code];
    }
    
    audioEngine->SetInterleavedBuffer(src, data, 1, AudioPlayer::GetBufferSize());   // they copy the contents of temp into their own buffer so giving stack memory is fine here
    audioEngine->SetSourceVolume(src, 1);   // the AudioAsset already applied the volume
    audioEngine->SetSourcePosition(src, worldpos.x, worldpos.y, worldpos.z);
    audioEngine->SetSourceRotation(src, worldrot.x, worldrot.y, worldrot.z, worldrot.w);
    audioEngine->SetSourceRoomEffectsGain(src, gain);
}


void AudioRoom::RoomData::Simulate(PlanarSampleBufferInlineView& buffer, PlanarSampleBufferInlineView& scratchBuffer){
    auto nchannels = AudioPlayer::GetNChannels();
    
    // convert to an array of pointers for Resonance
    stackarray(allchannelptrs, float*, nchannels);
    for(uint8_t i = 0; i < nchannels; i++){
        allchannelptrs[i] = buffer[i].data();
    }
    
    audioEngine->FillPlanarOutputBuffer(nchannels, buffer.sizeOneChannel(), allchannelptrs);
    AudioGraphComposed::Render(buffer, scratchBuffer, nchannels); // process graph
	
	// destroy sources
	for(const auto& source : allSources){
		audioEngine->DestroySource(source.second);
	}
	allSources.clear();
}

//void RavEngine::AudioRoom::DebugDraw(RavEngine::DebugDrawer& dbg, const RavEngine::Transform& tr) const
//{
//	dbg.DrawRectangularPrism(tr.CalculateWorldMatrix(), debug_color, data->roomDimensions);
//}
