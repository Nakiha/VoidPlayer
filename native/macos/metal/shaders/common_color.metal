
	float3 linear_to_srgb(float3 x) {
	  x = max(x, 0.0);
	  float3 lo = x * 12.92;
	  float3 hi = 1.055 * pow(x, 1.0 / 2.4) - 0.055;
	  return mix(lo, hi, step(0.0031308, x));
	}

	float3 srgb_to_linear(float3 x) {
	  x = saturate(x);
	  float3 lo = x / 12.92;
	  float3 hi = pow((x + 0.055) / 1.055, 2.4);
	  return mix(lo, hi, step(0.04045, x));
	}

	constant float kHDRReferenceWhiteNits = 203.0;
	constant float kHLGEDRHeadroomScale = 4.0;

	float3 convert_linear_bt2020_to_display_p3(float3 rgb) {
	  return float3(
	      1.3435782526 * rgb.r - 0.2821796705 * rgb.g - 0.0613985821 * rgb.b,
	     -0.0652974528 * rgb.r + 1.0757879158 * rgb.g - 0.0104904631 * rgb.b,
	      0.0028217873 * rgb.r - 0.0195984945 * rgb.g + 1.0167767073 * rgb.b);
	}

	float3 convert_linear_bt709_to_display_p3(float3 rgb) {
	  return float3(
	      0.8224619687 * rgb.r + 0.1775380313 * rgb.g,
	      0.0331941989 * rgb.r + 0.9668058011 * rgb.g,
	      0.0170826307 * rgb.r + 0.0723974407 * rgb.g + 0.9105199286 * rgb.b);
	}

	float3 convert_linear_bt601_to_display_p3(float3 rgb) {
	  return float3(
	      0.7758928495 * rgb.r + 0.2127372197 * rgb.g + 0.0113699286 * rgb.b,
	      0.0483696384 * rgb.r + 0.9353998726 * rgb.g + 0.0162304897 * rgb.b,
	      0.0158600140 * rgb.r + 0.0667994164 * rgb.g + 0.9173405701 * rgb.b);
	}

	float3 convert_linear_primaries_to_display_p3(float3 rgb, int primaries) {
	  if (primaries == kColorPrimariesBT2020) {
	    return convert_linear_bt2020_to_display_p3(rgb);
	  }
	  if (primaries == kColorPrimariesBT601) {
	    return convert_linear_bt601_to_display_p3(rgb);
	  }
	  return convert_linear_bt709_to_display_p3(rgb);
	}

	float3 convert_linear_primaries_to_bt709(float3 rgb, int primaries) {
	  if (primaries == kColorPrimariesBT2020) {
	    return float3(
	        1.6605 * rgb.r - 0.5876 * rgb.g - 0.0728 * rgb.b,
	       -0.1246 * rgb.r + 1.1329 * rgb.g - 0.0083 * rgb.b,
	       -0.0182 * rgb.r - 0.1006 * rgb.g + 1.1187 * rgb.b);
	  }
	  return rgb;
	}

	float3 pq_to_linear_nits(float3 x) {
	  x = saturate(x);
	  const float m1 = 0.1593017578125;
	  const float m2 = 78.84375;
	  const float c1 = 0.8359375;
	  const float c2 = 18.8515625;
	  const float c3 = 18.6875;
	  float3 p = pow(x, 1.0 / m2);
	  float3 num = max(p - c1, 0.0);
	  float3 den = max(c2 - c3 * p, 1e-6);
	  return pow(num / den, 1.0 / m1) * 10000.0;
	}

	float3 hlg_to_linear(float3 x) {
	  x = saturate(x);
	  const float a = 0.17883277;
	  const float b = 0.28466892;
	  const float c = 0.55991073;
	  float3 lo = (x * x) / 3.0;
	  float3 hi = (exp((x - c) / a) + b) / 12.0;
	  return mix(lo, hi, step(0.5, x));
	}

	float3 tone_map_to_sdr(float3 rgb, int transfer, int primaries) {
	  if (transfer == kColorTransferPQ) {
	    float3 lin = pq_to_linear_nits(rgb) / kHDRReferenceWhiteNits;
	    lin = convert_linear_primaries_to_bt709(lin, primaries);
	    return saturate(linear_to_srgb(lin / (1.0 + lin)));
	  }
	  if (transfer == kColorTransferHLG) {
	    float3 lin = hlg_to_linear(rgb) * kHLGEDRHeadroomScale;
	    lin = convert_linear_primaries_to_bt709(lin, primaries);
	    return saturate(linear_to_srgb(lin / (1.0 + lin)));
	  }
	  if (primaries == kColorPrimariesBT2020) {
	    float3 lin = convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries);
	    return saturate(linear_to_srgb(lin));
	  }
	  return saturate(rgb);
	}

	float3 map_to_edr(float3 rgb, int transfer, int primaries) {
	  if (transfer == kColorTransferPQ) {
	    float3 lin = pq_to_linear_nits(rgb) / kHDRReferenceWhiteNits;
	    return max(convert_linear_primaries_to_display_p3(lin, primaries), 0.0);
	  }
	  if (transfer == kColorTransferHLG) {
	    float3 lin = hlg_to_linear(rgb) * kHLGEDRHeadroomScale;
	    return max(convert_linear_primaries_to_display_p3(lin, primaries), 0.0);
	  }
	  return max(convert_linear_primaries_to_display_p3(srgb_to_linear(rgb), primaries), 0.0);
	}

	float3 map_to_output(float3 rgb,
	                     int transfer,
	                     int primaries,
	                     bool output_edr) {
	  if (output_edr) {
	    return map_to_edr(rgb, transfer, primaries);
	  }
	  return tone_map_to_sdr(rgb, transfer, primaries);
	}

	float yuv_sample_to_float(device const uchar* source, uint offset, bool is_p010) {
	  if (is_p010) {
	    uint lo = uint(source[offset]);
	    uint hi = uint(source[offset + 1]);
	    uint sample = ((hi << 8) | lo) >> 6;
	    return float(sample) / 1023.0;
	  }
	  return float(source[offset]) / 255.0;
	}

	float4 sample_yuv_track(device const uchar* source,
	                        constant LayoutParams& params,
	                        uint track_slot,
	                        uint source_x,
	                        uint source_y) {
	  const int format = yuv_format_at(params, track_slot);
	  const bool is_p010 = format == kPresentFormatP010;
	  const bool is_planar_yuv420 = format == kPresentFormatYUV420P;
	  const uint bytes_per_sample = is_p010 ? 2u : 1u;
	  const uint coded_width = uint(max(coded_width_at(params, track_slot), 1));
	  const uint coded_height = uint(max(coded_height_at(params, track_slot), 1));
	  const uint y_x = min(source_x, coded_width - 1);
	  const uint y_y = min(source_y, coded_height - 1);
	  const uint chroma_width = max((coded_width + 1u) / 2u, 1u);
	  const uint chroma_height = max((coded_height + 1u) / 2u, 1u);
	  const uint uv_x = min(y_x / 2u, chroma_width - 1u);
	  const uint uv_y = min(y_y / 2u, chroma_height - 1u);
	  const uint y_offset =
	      y_offset_at(params, track_slot) + y_y * y_stride_at(params, track_slot) +
	      y_x * bytes_per_sample;
	  const uint uv_offset = uv_offset_at(params, track_slot) +
	      uv_y * uv_stride_at(params, track_slot) +
	      uv_x * bytes_per_sample * (is_planar_yuv420 ? 1u : 2u);
	  const uint v_offset = is_planar_yuv420
	      ? v_offset_at(params, track_slot) +
	            uv_y * uv_stride_at(params, track_slot) +
	            uv_x * bytes_per_sample
	      : uv_offset + bytes_per_sample;
	  const float y = yuv_sample_to_float(source, y_offset, is_p010);
	  const float u = yuv_sample_to_float(source, uv_offset, is_p010);
	  const float v = yuv_sample_to_float(source, v_offset, is_p010);
	  float y_full = y;
	  float2 cbcr = (float2(u, v) * 255.0 - 128.0) / 255.0;
	  if (color_range_at(params, track_slot) != kColorRangeFull) {
	    y_full = (y * 255.0 - 16.0) / 219.0;
	    cbcr = (float2(u, v) * 255.0 - 128.0) / 224.0;
	  }
	  float cb = cbcr.x;
	  float cr = cbcr.y;
	  float3 rgb;
	  const int matrix = color_matrix_at(params, track_slot);
	  if (matrix == kColorMatrixBT2020NCL) {
	    rgb = float3(
	        y_full + 1.4746 * cr,
	        y_full - 0.164553 * cb - 0.571353 * cr,
	        y_full + 1.8814 * cb);
	  } else if (matrix == kColorMatrixBT709 || matrix == kColorMatrixUnknown) {
	    rgb = float3(
	        y_full + 1.5748 * cr,
	        y_full - 0.187324 * cb - 0.468124 * cr,
	        y_full + 1.8556 * cb);
	  } else {
	    rgb = float3(
	        y_full + 1.402 * cr,
	        y_full - 0.344136 * cb - 0.714136 * cr,
	        y_full + 1.772 * cb);
	  }
	  if (color_transfer_at(params, track_slot) == kColorTransferSDR) {
	    rgb -= (1.0 / 255.0);
	  }
	  return float4(
	      map_to_output(rgb,
	                    color_transfer_at(params, track_slot),
	                    color_primaries_at(params, track_slot),
	                    params.output_edr != 0),
	      1.0);
	}
