#pragma once
#include "mathtypes.hpp"

namespace RavEngine{
typedef uint32_t color_t;

struct Vertex
{
	float position[3];
};

struct UV
{
    float uv[2];
};

struct VertexNormalUV{
	float position[3];
	float normal[3];
	float uv[2];
};

struct VertexUV{
	float position[3];
	float uv[2];
};

struct VertexColor{
	float position[3];
	color_t color;
};

struct VertexColorUV {
	float position[3];
	float uv[2];
	color_t color;
};

struct Transformation{
	vector3 position = vector3(0,0,0);
	quaternion rotation = quaternion(1.0,0.0,0.0,0.0);
	vector3 scale = vector3(1,1,1);
	inline operator matrix4() const{
		return glm::translate(matrix4(1), (vector3)position) * glm::toMat4((quaternion)rotation) * glm::scale(matrix4(1), (vector3)scale);
	}
};

struct ColorRGBA{
	float R;
	float G;
	float B;
	float A;
};

/**
 Copy an array of type T to an array of type U
 @param input the source array
 @param output the destination array
 @param size optional size
 */
template<typename T, typename U>
constexpr static inline void copyMat4(const T* input, U* output, int size = 16) {
	for (int i = 0; i < size; i++) {
		output[i] = static_cast<U>(input[i]);
	}
}

/**
 @param x the number to round
 @param B the multiple base
 @return the closest multiple of B to x in the upwards direction. If x is already a multiple of B, returns x.
 */
inline constexpr int closest_multiple_of(int x, int B) {
	return ((x - 1) | (B - 1)) + 1;
}

/**
 @param x the number to round
 @param p the power
 @return x rounded up to the nearest power of p
*/
inline uint32_t closest_power_of(uint32_t x, uint32_t p) {
	return pow(p, ceil(log(x) / log(p)));
}
}

/**
 @param val an angle in degrees
 @return the angle in radians
 */
inline decimalType deg_to_rad(decimalType val){
    return glm::radians(val);
}

/**
 Perform a slerp between quaternions x and y
 @param x first target
 @param y second target
 @param a position
 @return posed quaternion
 */
inline auto Slerp(const quaternion& x, const quaternion& y, decimalType a){
    return glm::slerp(x,y,a);
}

template<typename S>
inline auto Slerp(const quaternion& x, const quaternion& y, decimalType a, S k){
    return glm::slerp(x,y,a,k);
}

struct Range {
	uint32_t start = 0, count = 0;
};