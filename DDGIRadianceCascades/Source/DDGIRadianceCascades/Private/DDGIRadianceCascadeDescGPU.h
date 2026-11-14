#pragma once

#include "ShaderParameterStruct.h"

/**
 * Compact GPU-facing Radiance Cascades configuration.
 *
 * The per-cascade intervals and packed ray offsets are derived from these
 * values in RadianceCascadeLayout.ush. This descriptor deliberately does
 * not reuse RTXGI's probe-grid descriptor.
 */
BEGIN_UNIFORM_BUFFER_STRUCT(FDDGIRadianceCascadeDescGPU, )
	SHADER_PARAMETER(int32, CascadeCount)
	SHADER_PARAMETER(int32, BaseRayCount)
	SHADER_PARAMETER(int32, RaySplitFactor)
	SHADER_PARAMETER(int32, TotalRaysPerProbe)
	SHADER_PARAMETER(float, BaseSegmentLength)
	SHADER_PARAMETER(float, CascadeScale)
	SHADER_PARAMETER(float, MaxTraceDistance)
END_UNIFORM_BUFFER_STRUCT()
