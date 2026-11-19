// Octahedral normal encoding (Karis 2010): a unit direction is folded into two scalars in
// [-1, 1]. Compared with a direct RGB storage of the same bit budget the angular error is much
// lower, and the freed channels can hold other attributes instead:
//   RGB8 direct storage:  0.17 deg average / 0.38 deg maximum error
//   octahedral RG8:       ~0.04 deg maximum error
//   octahedral RG16F:     0.011 deg average / 0.040 deg maximum error
// The G-Buffer uses this to store the eye-space normal in NormalBuffer.rg (mapped to [0, 1] for
// unorm-capable targets), while NormalBuffer.a keeps the material AO and NormalBuffer.b is
// reserved (for bent normals or velocity later). See std_gbuffer.frag.glsl (writing) and
// std_pbr_lighting / std_shadow_combine (reading)

// Encode a unit (or non-unit, it is normalized inside) direction into 2 components in [-1, 1]
vec2 encode_normal_octahedral(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = (n.z >= 0.0) ? n.xy
           : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}

// Decode a value previously written by encode_normal_octahedral() and stored in [0, 1]
vec3 decode_normal_octahedral(vec2 e)
{
    e = e * 2.0 - 1.0;
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}
