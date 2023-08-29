/**
Converts to the eye specific uv.
In VR, texture buffers include the left and right eye in the same buffer. Flat only has a single camera for the entire width.
This means the x value [0, .5] represents the left eye, and the y value (.5, 1] are the right eye.
This returns the adjusted value
@param uv - uv coords [0,1] to be encoded for VR
@param a_eyeIndex The eyeIndex; 0 is left, 1 is right
@returns uv with x coords adjusted for the VR texture buffer
*/
float2 ConvertToStereoUV(float2 uv, uint a_eyeIndex)
{
#ifdef VR
	// convert [0,1] to eye specific [0,.5] and [.5, 1] dependent on a_eyeIndex
	uv.x = (uv.x + (float)a_eyeIndex) / 2;
#endif
	return uv;
}

/**
Converts from eye specific uv to general uv.
In VR, texture buffers include the left and right eye in the same buffer. 
This means the x value [0, .5] represents the left eye, and the y value (.5, 1] are the right eye.
This returns the adjusted value
@param uv - eye specific uv coords [0,1]; if uv.x < 0.5, it's a left eye; otherwise right
@param a_eyeIndex The eyeIndex; 0 is left, 1 is right
@returns uv with x coords adjusted to full range for either left or right eye
*/
float2 ConvertFromStereoUV(float2 uv, uint a_eyeIndex)
{
#ifdef VR
	// convert [0,.5] to [0, 1] and [.5, 1] to [0,1]
	uv.x = 2 * uv.x - (float)a_eyeIndex;
#endif
	return uv;
}
