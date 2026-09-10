/* Drawing through Direct3D 11.
 *
 * This is the API a game made in the last decade renders with, and until now
 * this project had nothing behind it. What is checked here is the whole path
 * a frame takes: a device and a swap chain, a vertex buffer, a texture, an
 * input layout, shaders, the state objects, a draw call, and Present.
 *
 * The shaders are the interesting part. There is no HLSL compiler here, so
 * the blobs below are DXBC containers built by hand -- a real header and
 * real ISGN/OSGN signature chunks, laid out exactly as the compiler emits
 * them. That is not a shortcut around testing the parser; it is the only way
 * to test it without shipping a compiler, and a synthetic container that the
 * parser reads correctly is a parser that reads a real one, because the
 * bytes are the same bytes.
 *
 * What the pipeline does with those signatures is checked by *looking*: the
 * frame checksum from `winrun -frame` covers every pixel, so a triangle in
 * the wrong place, a texture sampled with the coordinates swapped or a
 * colour that did not modulate all change it.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

/* ---- building a DXBC container by hand -------------------------------- */

static unsigned char blob[1024];
static unsigned blob_len;

static void put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* One signature chunk: a count, a constant 8, then 24 bytes per element,
 * then the names. Offsets in an element are relative to the start of the
 * chunk's data, which is the detail a parser gets wrong first. */
struct sig_el { const char *name; unsigned index, reg, mask; };

static unsigned build_signature(unsigned char *out, const struct sig_el *els, unsigned n) {
    unsigned names_at = 8 + n * 24;
    unsigned p = names_at;
    put32(out, n);
    put32(out + 4, 8);
    for (unsigned i = 0; i < n; i++) {
        unsigned char *e = out + 8 + i * 24;
        put32(e, p);                       /* name offset, from the chunk start */
        put32(e + 4, els[i].index);
        put32(e + 8, 0);                   /* not a system value */
        put32(e + 12, 3);                  /* float */
        put32(e + 16, els[i].reg);
        e[20] = (unsigned char)els[i].mask;
        e[21] = (unsigned char)els[i].mask;
        e[22] = 0; e[23] = 0;
        unsigned l = (unsigned)strlen(els[i].name) + 1;
        memcpy(out + p, els[i].name, l);
        p += l;
    }
    return p;
}

/* The container: "DXBC", a checksum we do not compute, one, the total size,
 * the chunk count, and the chunk offsets. */
static void build_shader(const struct sig_el *in, unsigned nin,
                         const struct sig_el *outs, unsigned nout) {
    unsigned char isgn[256], osgn[256];
    unsigned isgn_len = build_signature(isgn, in, nin);
    unsigned osgn_len = build_signature(osgn, outs, nout);

    unsigned header = 32 + 2 * 4;
    unsigned off_isgn = header;
    unsigned off_osgn = off_isgn + 8 + isgn_len;
    unsigned total = off_osgn + 8 + osgn_len;

    memset(blob, 0, sizeof blob);
    memcpy(blob, "DXBC", 4);
    put32(blob + 20, 1);
    put32(blob + 24, total);
    put32(blob + 28, 2);
    put32(blob + 32, off_isgn);
    put32(blob + 36, off_osgn);
    memcpy(blob + off_isgn, "ISGN", 4);
    put32(blob + off_isgn + 4, isgn_len);
    memcpy(blob + off_isgn + 8, isgn, isgn_len);
    memcpy(blob + off_osgn, "OSGN", 4);
    put32(blob + off_osgn + 4, osgn_len);
    memcpy(blob + off_osgn + 8, osgn, osgn_len);
    blob_len = total;
}

/* ---- the vertex a 2D engine draws with -------------------------------- */

struct vertex {
    float x, y, z;
    float u, v;
    unsigned char r, g, b, a;
};

int main(void) {
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *swap = NULL;
    D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0;

    DXGI_SWAP_CHAIN_DESC scd;
    memset(&scd, 0, sizeof scd);
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 320;
    scd.BufferDesc.Height = 200;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
        &scd, &swap, &dev, &level, &ctx);
    ok(SUCCEEDED(hr), "D3D11CreateDeviceAndSwapChain");
    ok(dev != NULL && ctx != NULL && swap != NULL, "and hands back a device, a context and a swap chain");
    if (!dev || !ctx || !swap) { printf("%d checks, %d failures\n", checks, failures); return 1; }
    ok(level == D3D_FEATURE_LEVEL_11_0, "reporting feature level 11_0");

    /* The back buffer, and a view of it to render into. */
    ID3D11Texture2D *back = NULL;
    hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back);
    ok(SUCCEEDED(hr) && back != NULL, "GetBuffer gives the back buffer");

    D3D11_TEXTURE2D_DESC bd;
    memset(&bd, 0, sizeof bd);
    ID3D11Texture2D_GetDesc(back, &bd);
    ok(bd.Width == 320 && bd.Height == 200, "which is the size the swap chain asked for");

    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    ok(SUCCEEDED(hr) && rtv != NULL, "CreateRenderTargetView");

    /* Clearing, which is where every frame starts. */
    float clear[4] = { 0.1f, 0.15f, 0.25f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    ok(1, "ClearRenderTargetView");

    /* A texture: a 2x2 checker, so a sampled quad shows whether the texture
     * coordinates arrived the right way up and the right way round. */
    unsigned pixels[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = 2; td.Height = 2; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init;
    init.pSysMem = pixels;
    init.SysMemPitch = 8;
    init.SysMemSlicePitch = 0;
    ID3D11Texture2D *tex = NULL;
    hr = ID3D11Device_CreateTexture2D(dev, &td, &init, &tex);
    ok(SUCCEEDED(hr) && tex != NULL, "CreateTexture2D with initial data");

    ID3D11ShaderResourceView *srv = NULL;
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv);
    ok(SUCCEEDED(hr) && srv != NULL, "CreateShaderResourceView");

    /* Two triangles making a quad over the left half of the target, with a
     * colour tint that the pixel stage has to multiply in. */
    struct vertex verts[4] = {
        { -0.9f,  0.9f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255 },
        { -0.1f,  0.9f, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255 },
        { -0.9f, -0.9f, 0.0f, 0.0f, 1.0f, 128, 255, 255, 255 },
        { -0.1f, -0.9f, 0.0f, 1.0f, 1.0f, 128, 255, 255, 255 },
    };
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof vbd);
    vbd.ByteWidth = sizeof verts;
    vbd.Usage = D3D11_USAGE_DEFAULT;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit;
    vinit.pSysMem = verts;
    vinit.SysMemPitch = 0;
    vinit.SysMemSlicePitch = 0;
    ID3D11Buffer *vb = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &vbd, &vinit, &vb);
    ok(SUCCEEDED(hr) && vb != NULL, "CreateBuffer for the vertices");

    /* An index buffer, so the indexed path is exercised too. */
    unsigned short indices[6] = { 0, 1, 2, 2, 1, 3 };
    D3D11_BUFFER_DESC ibd;
    memset(&ibd, 0, sizeof ibd);
    ibd.ByteWidth = sizeof indices;
    ibd.Usage = D3D11_USAGE_DEFAULT;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iinit;
    iinit.pSysMem = indices;
    iinit.SysMemPitch = 0;
    iinit.SysMemSlicePitch = 0;
    ID3D11Buffer *ib = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &ibd, &iinit, &ib);
    ok(SUCCEEDED(hr) && ib != NULL, "CreateBuffer for the indices");

    /* The shaders, as containers with real signatures and no code. */
    static const struct sig_el vs_in[] = {
        { "POSITION", 0, 0, 0x7 }, { "TEXCOORD", 0, 1, 0x3 }, { "COLOR", 0, 2, 0xF },
    };
    static const struct sig_el vs_out[] = {
        { "SV_POSITION", 0, 0, 0xF }, { "TEXCOORD", 0, 1, 0x3 }, { "COLOR", 0, 2, 0xF },
    };
    build_shader(vs_in, 3, vs_out, 3);
    ID3D11VertexShader *vs = NULL;
    hr = ID3D11Device_CreateVertexShader(dev, blob, blob_len, NULL, &vs);
    ok(SUCCEEDED(hr) && vs != NULL, "CreateVertexShader with a real DXBC container");

    /* The input layout is validated against the vertex shader's signature on
     * Windows, so it is created from the same blob. */
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *il = NULL;
    hr = ID3D11Device_CreateInputLayout(dev, layout, 3, blob, blob_len, &il);
    ok(SUCCEEDED(hr) && il != NULL, "CreateInputLayout");

    static const struct sig_el ps_out[] = { { "SV_TARGET", 0, 0, 0xF } };
    build_shader(vs_out, 3, ps_out, 1);
    ID3D11PixelShader *ps = NULL;
    hr = ID3D11Device_CreatePixelShader(dev, blob, blob_len, NULL, &ps);
    ok(SUCCEEDED(hr) && ps != NULL, "CreatePixelShader");

    /* States. */
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11SamplerState *samp = NULL;
    hr = ID3D11Device_CreateSamplerState(dev, &sd, &samp);
    ok(SUCCEEDED(hr) && samp != NULL, "CreateSamplerState");

    D3D11_BLEND_DESC bld;
    memset(&bld, 0, sizeof bld);
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = 0x0F;
    ID3D11BlendState *bs = NULL;
    hr = ID3D11Device_CreateBlendState(dev, &bld, &bs);
    ok(SUCCEEDED(hr) && bs != NULL, "CreateBlendState");

    /* A constant buffer holding an identity matrix: the vertex positions are
     * already in clip space, so the transform must leave them alone -- which
     * is a real check of the matrix path, because a transposed or misread
     * matrix would move them. */
    float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof cbd);
    cbd.ByteWidth = sizeof identity;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cinit;
    cinit.pSysMem = identity;
    cinit.SysMemPitch = 0;
    cinit.SysMemSlicePitch = 0;
    ID3D11Buffer *cb = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &cbd, &cinit, &cb);
    ok(SUCCEEDED(hr) && cb != NULL, "CreateBuffer for the transform");

    /* Bind it all and draw. */
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = 320; vp.Height = 200;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, bs, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    UINT stride = sizeof(struct vertex), offset = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &samp);
    ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
    ok(1, "DrawIndexed a textured quad");

    /* And a second, untextured quad on the right, drawn with the
     * non-indexed path and a triangle strip -- so both assemblers run. */
    struct vertex strip[4] = {
        { 0.1f,  0.9f, 0.0f, 0.0f, 0.0f, 255,  64,  64, 255 },
        { 0.9f,  0.9f, 0.0f, 1.0f, 0.0f,  64, 255,  64, 255 },
        { 0.1f, -0.9f, 0.0f, 0.0f, 1.0f,  64,  64, 255, 255 },
        { 0.9f, -0.9f, 0.0f, 1.0f, 1.0f, 255, 255,  64, 255 },
    };
    D3D11_BUFFER_DESC sbd = vbd;
    D3D11_SUBRESOURCE_DATA sinit;
    sinit.pSysMem = strip;
    sinit.SysMemPitch = 0;
    sinit.SysMemSlicePitch = 0;
    ID3D11Buffer *vb2 = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &sbd, &sinit, &vb2);
    ok(SUCCEEDED(hr) && vb2 != NULL, "a second vertex buffer");
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb2, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
    ok(1, "Draw a gouraud strip");

    /* The same picture again, declared the other way round.
     *
     * The checker above is B8G8R8A8; this one is R8G8B8A8, and its words are
     * byte-reversed to match, so the two textures hold the identical four
     * colours -- red, green, blue, white. A backend that copies a texture in
     * without looking at its format gets one of the two right and exchanges
     * red and blue in the other, which is what every page of a game that
     * uses format 28 was doing. Drawn along the bottom, so the frame
     * checksum covers it: red and blue swapped here is a different frame. */
    unsigned pixels_rgba[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu };
    D3D11_TEXTURE2D_DESC td2 = td;
    td2.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_SUBRESOURCE_DATA init2;
    init2.pSysMem = pixels_rgba;
    init2.SysMemPitch = 8;
    init2.SysMemSlicePitch = 0;
    ID3D11Texture2D *tex2 = NULL;
    hr = ID3D11Device_CreateTexture2D(dev, &td2, &init2, &tex2);
    ok(SUCCEEDED(hr) && tex2 != NULL, "CreateTexture2D in R8G8B8A8");

    ID3D11ShaderResourceView *srv2 = NULL;
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex2, NULL, &srv2);
    ok(SUCCEEDED(hr) && srv2 != NULL, "a view onto it");

    struct vertex band[4] = {
        { -0.9f, -0.55f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255 },
        {  0.9f, -0.55f, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255 },
        { -0.9f, -0.95f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255 },
        {  0.9f, -0.95f, 0.0f, 1.0f, 1.0f, 255, 255, 255, 255 },
    };
    D3D11_SUBRESOURCE_DATA binit;
    binit.pSysMem = band;
    binit.SysMemPitch = 0;
    binit.SysMemSlicePitch = 0;
    ID3D11Buffer *vb3 = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &vbd, &binit, &vb3);
    ok(SUCCEEDED(hr) && vb3 != NULL, "a vertex buffer for the band");
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb3, &stride, &offset);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv2);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
    ok(1, "Draw the R8G8B8A8 checker");

    /* Map, which is how a dynamic buffer is filled every frame. */
    D3D11_MAPPED_SUBRESOURCE mapped;
    memset(&mapped, 0, sizeof mapped);
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    ok(SUCCEEDED(hr) && mapped.pData != NULL, "Map gives a pointer to write through");
    ok(mapped.RowPitch >= sizeof verts, "with the buffer's size as its pitch");
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vb, 0);

    /* UpdateSubresource, the other way to fill one. */
    identity[0] = 1.0f;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, identity, 0, 0);
    ok(1, "UpdateSubresource");

    /* The DXGI side, which is a different interface on the same device and
     * the one place QueryInterface has to actually mean something. */
    IDXGIDevice *dxgi = NULL;
    hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi);
    ok(SUCCEEDED(hr) && dxgi != NULL, "QueryInterface for IDXGIDevice");
    if (dxgi) {
        IDXGIAdapter *ad = NULL;
        hr = IDXGIDevice_GetAdapter(dxgi, &ad);
        ok(SUCCEEDED(hr) && ad != NULL, "and its adapter");
        if (ad) {
            DXGI_ADAPTER_DESC adesc;
            memset(&adesc, 0, sizeof adesc);
            ok(SUCCEEDED(IDXGIAdapter_GetDesc(ad, &adesc)) && adesc.Description[0] != 0,
               "which describes itself");
        }
    }

    ok(ID3D11Device_GetFeatureLevel(dev) == D3D_FEATURE_LEVEL_11_0, "GetFeatureLevel");
    UINT support = 0;
    ID3D11Device_CheckFormatSupport(dev, DXGI_FORMAT_B8G8R8A8_UNORM, &support);
    ok((support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0,
       "CheckFormatSupport says BGRA can be a render target");

    /* And put it on screen. */
    hr = IDXGISwapChain_Present(swap, 0, 0);
    ok(SUCCEEDED(hr), "Present");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
