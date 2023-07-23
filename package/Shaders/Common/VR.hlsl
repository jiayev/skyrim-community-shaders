/*
* Multiply for matrixes that will use an a_eyeIndex.
* This uses a standard mul if in flat
*
* @param a_matrix The matrix to multiple against
* @param a_vector The vector to multiply
* @param a_eyeIndex The a_eyeIndex, normally 0 for flat
* @return The result of a mul that works in VR with eyeindex
*/
float4 NG_mul(float4x4 a_matrix, float4 a_vector, uint a_eyeIndex = 0)
{
#if !defined(VR)
	float4 result = mul(a_matrix, a_vector);
#else
	float4 result;
	result.x = dot(a_matrix[a_eyeIndex + 0].xyzw, a_vector.xyzw);
	result.y = dot(a_matrix[a_eyeIndex + 1].xyzw, a_vector.xyzw);
	result.z = dot(a_matrix[a_eyeIndex + 2].xyzw, a_vector.xyzw);
	result.w = dot(a_matrix[a_eyeIndex + 3].xyzw, a_vector.xyzw);
#endif  // VR
	return result;
}