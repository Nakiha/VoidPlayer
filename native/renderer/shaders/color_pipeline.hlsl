// YUV -> RGB conversion and SDR output mapping.

float3 linear_to_srgb(float3 x) {
    x = max(x, 0.0);
    float3 lo = x * 12.92;
    float3 hi = 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, x));
}

float3 srgb_to_linear(float3 x) {
    x = saturate(x);
    float3 lo = x / 12.92;
    float3 hi = pow((x + 0.055) / 1.055, 2.4);
    return lerp(lo, hi, step(0.04045, x));
}

float3 convert_linear_primaries_to_bt709(float3 rgb, int primaries) {
    if (primaries == COLOR_PRIMARIES_BT2020) {
        return float3(
            1.6605 * rgb.r - 0.5876 * rgb.g - 0.0728 * rgb.b,
           -0.1246 * rgb.r + 1.1329 * rgb.g - 0.0083 * rgb.b,
           -0.0182 * rgb.r - 0.1006 * rgb.g + 1.1187 * rgb.b);
    }
    return rgb;
}

float3 pq_to_linear_nits(float3 x) {
    x = saturate(x);
    const float m1 = 0.1593017578125;  // 2610 / 16384
    const float m2 = 78.84375;         // 2523 / 32
    const float c1 = 0.8359375;        // 3424 / 4096
    const float c2 = 18.8515625;       // 2413 / 128
    const float c3 = 18.6875;          // 2392 / 128
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
    return lerp(lo, hi, step(0.5, x));
}

float3 tone_map_to_sdr(float3 rgb, int transfer, int primaries) {
    if (transfer == COLOR_TRANSFER_PQ) {
        float3 lin = pq_to_linear_nits(rgb) / 203.0;
        lin = convert_linear_primaries_to_bt709(lin, primaries);
        return saturate(linear_to_srgb(lin / (1.0 + lin)));
    }
    if (transfer == COLOR_TRANSFER_HLG) {
        float3 lin = hlg_to_linear(rgb) * 4.0;
        lin = convert_linear_primaries_to_bt709(lin, primaries);
        return saturate(linear_to_srgb(lin / (1.0 + lin)));
    }
    if (primaries == COLOR_PRIMARIES_BT2020) {
        float3 lin = convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries);
        return saturate(linear_to_srgb(lin));
    }
    return saturate(rgb);
}

float3 map_to_windows_scrgb(float3 rgb, int transfer, int primaries) {
    if (transfer == COLOR_TRANSFER_PQ) {
        return convert_linear_primaries_to_bt709(
            pq_to_linear_nits(rgb) / 80.0,
            primaries);
    }
    if (transfer == COLOR_TRANSFER_HLG) {
        return convert_linear_primaries_to_bt709(
            hlg_to_linear(rgb) * (4.0 * 203.0 / 80.0),
            primaries);
    }
    return convert_linear_primaries_to_bt709(
        srgb_to_linear(rgb),
        primaries) * u_sdr_white_scale;
}

float3 map_to_output(float3 rgb, int transfer, int primaries) {
    if (u_output_target == OUTPUT_TARGET_WINDOWS_SCRGB) {
        return map_to_windows_scrgb(rgb, transfer, primaries);
    }
    return tone_map_to_sdr(rgb, transfer, primaries);
}

float4 map_sdr_ui_to_output(float4 color) {
    color = saturate(color);
    if (u_output_target == OUTPUT_TARGET_WINDOWS_SCRGB) {
        return float4(srgb_to_linear(color.rgb) * u_sdr_white_scale, color.a);
    }
    return color;
}

float3 yuv_to_rgb(float y, float2 uv, int range, int color_matrix, int transfer, int primaries) {
    float y_full = y;
    float2 cbcr = (uv * 255.0 - 128.0) / 255.0;

    if (range != COLOR_RANGE_FULL) {
        y_full = (y * 255.0 - 16.0) / 219.0;
        cbcr = (uv * 255.0 - 128.0) / 224.0;
    }

    float cb = cbcr.x;
    float cr = cbcr.y;
    float3 rgb;
    if (color_matrix == COLOR_MATRIX_BT2020_NCL) {
        rgb = float3(
            y_full + 1.4746 * cr,
            y_full - 0.164553 * cb - 0.571353 * cr,
            y_full + 1.8814 * cb);
    } else if (color_matrix == COLOR_MATRIX_BT709 || color_matrix == COLOR_MATRIX_UNKNOWN) {
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

    if (transfer == COLOR_TRANSFER_SDR) {
        // Keep the shared YUV shader path on the same output rounding side as
        // the previous software decode presentation path for ordinary SDR.
        rgb -= (1.0 / 255.0);
    }

    return map_to_output(rgb, transfer, primaries);
}
