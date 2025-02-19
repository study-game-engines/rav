#pragma once
#include "ComponentWithOwner.hpp"
#include <ozz/animation/runtime/animation.h>
#include <ozz/base/maths/soa_transform.h>
#include "AnimationAsset.hpp"
#include "DataStructures.hpp"
#include "Ref.hpp"
#include <algorithm>
#include "Tween.hpp"
#include "App.hpp"
#include "Function.hpp"

namespace RavEngine{

struct Transform;
class SkeletonAsset;

class clamped_vec2{
	float x, y;
public:
	clamped_vec2(float ix, float iy) : x(std::clamp(ix,-1.0f,1.0f)), y(std::clamp(iy,-1.0f,1.0f)){}
	clamped_vec2() : clamped_vec2(0,0){}
	
	constexpr inline decltype(x) get_x() const{
		return x;
	}
    constexpr inline decltype(y) get_y() const{
		return y;
	}
};

struct AnimBlendTree : public IAnimGraphable{
	struct Node : public IAnimGraphable{
		Ref<IAnimGraphable> state;
		clamped_vec2 graph_pos;
		float max_influence = 1;
		
		Node(){}
		
		Node(const Node& other) : state(other.state), graph_pos(other.graph_pos), max_influence(other.max_influence){}
		
		template<typename T>
		Node(Ref<T> s, const clamped_vec2& pos, float i = 1) : state(std::static_pointer_cast<IAnimGraphable>(s)), graph_pos(pos), max_influence(i){}
		
		/**
		 Sample the animation curves in this tree
		 @param t the time to sample
		 @param output the vector to write the output transforms to
		 @param cache a sampling cache, modified when used
		 */
		bool Sample(float t, float start, float speed, bool looping, ozz::vector<ozz::math::SoaTransform>&, ozz::animation::SamplingJob::Context& cache, const ozz::animation::Skeleton* skeleton) const override;
	};

	static constexpr uint16_t kmax_nodes = 64;
	
	/**
	 Insert a node into the tree at the id. If a node already exists at that ID, it is replaced.
	 @param id the identifier for the node. Recommended to create an enum to use here.
	 @param node the node to insert into the tree.
	 */
    inline void InsertNode(uint8_t id, const Node& node){
		states[id].node = node;
	}
	
	/**
	 Remove a node given an ID
	 @param id the id to remove
	 */
    inline void DeleteNode(uint8_t id){
		states.erase(id);
	}
	
	/**
	 Get a node reference to make modifications to it
	 @param id the ID to get a node for
	 @returns node reference
	 @throws if no node exists at id
	 */
    Node& GetNode(const uint8_t id){
		return states.at(id).node;
	}
	
    inline bool IsEmpty() const{
		return states.empty();
	}
	
    inline void Clear(){
		states.clear();
	}
	
	/**
	 Sample the animation curves in this tree
	 @param t the time to sample
	 @param output the vector to write the output transforms to
	 @param cache a sampling cache, modified when used
	 */
	bool Sample(float t, float start, float speed, bool looping, ozz::vector<ozz::math::SoaTransform>&, ozz::animation::SamplingJob::Context& cache, const ozz::animation::Skeleton* skeleton) const override;
	
    constexpr inline void SetBlendPos(const clamped_vec2& newPos){
		blend_pos = newPos;
	}
	
private:
	struct Sampler{
		ozz::vector<ozz::math::SoaTransform> locals;
		Node node;
	};
	locked_node_hashmap<uint8_t,Sampler,SpinLock> states;
	clamped_vec2 blend_pos;
};

class AnimatorComponent : public ComponentWithOwner, public IDebugRenderable, public Queryable<AnimatorComponent,IDebugRenderable>{
protected:
	double lastPlayTime = 0;
	Ref<SkeletonAsset> skeleton;
public:
	//a node in the state machine
	struct State{
		friend class AnimatorComponent;
		unsigned short ID;
		Ref<IAnimGraphable> clip;
		bool isLooping = true;
		float speed = 1;
		
		double lastPlayTime = 0;

		struct Transition{
			enum class TimeMode{
				Blended = 0,	//the time from this state carries over to the target state
				BeginNew = 1	//the target state's time is set to 0 when the transition begins
			} type;
			tweeny::tween<float> transition;
		};
		
		//transitions out of this state, keyed by ID
		UnorderedMap<decltype(ID),Transition> exitTransitions;
		
		template<typename T, typename decimal>
        constexpr inline State& SetTransition(decltype(ID) id, T interpolation, decimal duration, Transition::TimeMode mode = Transition::TimeMode::Blended){
			auto tween = tweeny::from(0.0f).to(1.0f).during(static_cast<float>(duration * GetApp()->evalNormal)).via(interpolation);
			exitTransitions[id].transition = tween;
			exitTransitions[id].type = mode;
			return *this;
		}

		/**
		* Construct a State
		*/
        State(decltype(ID) ID, decltype(clip) clip, decltype(isLooping) il = true, decltype(speed) speed = 1) : ID(ID), clip(clip), isLooping(il), speed(speed) {}

		State() {}

	private:
		bool hasAutoTransition = false;
		decltype(ID) autoTransitionID = 0;
		Function<void(decltype(State::ID))> beginCallback, endCallback;

        inline void DoBegin(decltype(ID) prevState) {
			if (beginCallback) {
				beginCallback(prevState);
			}
		}

        inline void DoEnd(decltype(ID) nextState) {
			if (endCallback) {
				endCallback(nextState);
			}
		}
	public:

		/**
		* When this animation completes, it will automatically transition to the state provided to this call.
		* @note if this state is looping, it will never automatically leave.
		* @param id the numeric ID to transition to. The blending curve and rules use SetTransition as normal.
		*/
        constexpr inline void SetAutoTransition(decltype(ID) id) {
			hasAutoTransition = true;
			autoTransitionID = id;
		}

		/**
		* Clear any active auto transition.
		*/
        constexpr inline void ClearAutoTransition() {
			hasAutoTransition = false;
		}

		/**
		* Set the function to call when this state begins.
		* @param bc function taking one parameter representing the ID of the previous state. This may be invalid the first time it is called.
		*/
        inline void SetBeginCallback(const decltype(beginCallback)& bc) {
			beginCallback = bc;
		}

		/**
		* Set the function to call when this state ends (has finished playing, or is interrupted and moving to a new state).
		* @param bc function taking one parameter representing the ID of the next state. If the state machine is not transitioning, the ID will be that of the current state. 
		*/
        inline void SetEndCallback(const decltype(endCallback)& ec) {
			endCallback = ec;
		}

        constexpr inline bool HasAutoTransition() const{
			return hasAutoTransition;
		}

        constexpr inline decltype(autoTransitionID) GetAutoTransitionID() const {
			return autoTransitionID;
		}
	};
	
	typedef decltype(State::ID) id_t;
	
	/**
	 Create an AnimatorComponent with a SkeletonAsset
	 @param sk the skeleton asset
	 */
	AnimatorComponent(entity_t owner, Ref<SkeletonAsset> sk);
		
	/**
	 Transitions to the new state. If the current state has a transition to the target state, that transition is played.
	 Otherwise, the state machine simply jumps to the target state without a transition.
	 @param newState the state to switch to
	 */
    void Goto(id_t newState, bool skipTransition = false);
	
	/**
	 Add a state to the state machine
	 @param state the state to insert
	 */
    inline void InsertState(const State& state){
		states.insert(std::make_pair(state.ID,state));
	}
	
	/**
	 Begin playing this AnimatorController
	 @param resetPlayhead true if the time of this animator should be reset (for nonlooping animations), false to resume where paused (for looping animations)
	 */
    void Play(float resetPlayhead = false);
	
    void Pause();

    void Tick();
	
    inline decltype(skeleton) GetSkeleton() const{
		return skeleton;
	}
    
    virtual void DebugDraw(RavEngine::DebugDrawer& dbg, const Transform&) const override;
	
	void UpdateSocket(const std::string&, Transform&) const;

protected:
	locked_node_hashmap<id_t,State> states;
	
	struct StateBlend{
		id_t from, to;
		decltype(State::Transition::transition) currentTween;
	} stateBlend;
		
	id_t currentState = 0;
	
	ozz::vector<ozz::math::SoaTransform> transforms, transformsSecondaryBlending;
    std::shared_ptr<ozz::animation::SamplingJob::Context> cache = std::make_shared<ozz::animation::SamplingJob::Context>();
	ozz::vector<ozz::math::Float4x4> models;
    mutable ozz::vector<matrix4> glm_pose;
	ozz::vector<matrix4> local_pose;
	ozz::vector<matrix4> skinningmats;
	
	/**
	 Update buffer sizes for current skeleton
	 */
	void UpdateSkeletonData(Ref<SkeletonAsset> sk);
	
	bool isPlaying : 1;
	bool isBlending : 1;
	float currentBlendingValue = 0;

	inline void EndState(State& state, decltype(State::ID) nextState) {
		state.DoEnd(nextState);
		if (state.HasAutoTransition()) {
			Goto(state.GetAutoTransitionID());
		}
	}

public:
	/**
	* @return the ID of the state the animator is currently playing
	*/
	inline decltype(currentState) GetCurrentState() const{
		return currentState;
	}

	/**
	 Get the current pose of the animation in world space
	 @return vector of matrices representing the world-space transformations of every joint in the skeleton for the current animation frame
	 */
	const decltype(glm_pose)& GetPose() const;
	
	const decltype(local_pose)& GetLocalPose();
	
	inline const decltype(skinningmats)& GetSkinningMats(){
		return skinningmats;
	}
    
};

}
