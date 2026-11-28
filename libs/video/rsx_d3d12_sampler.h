#ifndef RSX_D3D12_SAMPLER_H
#define RSX_D3D12_SAMPLER_H

static D3D12_TEXTURE_ADDRESS_MODE rsx_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;    /* CLAMP_TO_EDGE, CLAMP */
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 6: case 7: case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;            /* 1 = WRAP */
    }
}

/* Sampler from the guest's TEXTURE_ADDRESS (wrap S [3:0], T [11:8], R [19:16]),
 * TEXTURE_FILTER (LOD bias [12:0] signed x/256, min [18:16], mag [26:24]) and
 * TEXTURE_CONTROL0 (max anisotropy [6:4]) and BORDER_COLOR (ARGB). */
static void rsx_sampler_desc(const u32 k[4], D3D12_SAMPLER_DESC* s)
{
    const u32 addr = k[0], filter = k[1], ctrl0 = k[2], border = k[3];
    const u32 minf = (filter >> 16) & 7u, magf = (filter >> 24) & 7u;
    const int min_lin = minf == 2 || minf == 4 || minf == 6 || minf == 7;
    const int mip_lin = minf == 5 || minf == 6 || minf == 7;
    const int mag_lin = magf == 2 || magf == 4;
    static const u8 aniso[8] = { 1, 2, 4, 6, 8, 10, 12, 16 };
    memset(s, 0, sizeof *s);
    if (aniso[(ctrl0 >> 4) & 7u] > 1 && min_lin && mag_lin) {
        s->Filter = D3D12_FILTER_ANISOTROPIC;
        s->MaxAnisotropy = aniso[(ctrl0 >> 4) & 7u];
    } else {
        s->Filter = (D3D12_FILTER)((min_lin << 4) | (mag_lin << 2) | mip_lin);
        s->MaxAnisotropy = 1;
    }
    s->AddressU = rsx_wrap(addr);
    s->AddressV = rsx_wrap(addr >> 8);
    s->AddressW = rsx_wrap(addr >> 16);
    int bias = (int)(filter & 0x1FFFu);
    if (bias & 0x1000) bias -= 0x2000;
    s->MipLODBias = (float)bias / 256.0f;
    s->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s->BorderColor[0] = (float)((border >> 16) & 0xFFu) / 255.0f;
    s->BorderColor[1] = (float)((border >> 8) & 0xFFu) / 255.0f;
    s->BorderColor[2] = (float)(border & 0xFFu) / 255.0f;
    s->BorderColor[3] = (float)(border >> 24) / 255.0f;
    s->MinLOD = (float)((ctrl0 >> 19) & 0xFFFu) / 256.0f;
    s->MaxLOD = (minf == 1 || minf == 2) ? s->MinLOD : (float)((ctrl0 >> 7) & 0xFFFu) / 256.0f;
    if (s->MaxLOD < s->MinLOD) s->MaxLOD = s->MinLOD;
}

#endif
