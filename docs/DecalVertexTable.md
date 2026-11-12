# DecalVertexTable

A footprint, a blood splat or a scorch mark lands on a certain kind of mesh and the game crashes, on the spot or minutes later somewhere unrelated.

The direct form is `SkyrimSE.exe+0E53F5D`, `movss xmm2, [r12+rax*8]` with a very large RAX, under `BSTempEffectSimpleDecal::Initialize` on the main thread. The indirect form has no fixed address, because the same bug writes into the heap. The follow-up crash shows up in the decal's tangent code, in a particle modifier, in the memory allocator and in the renderer.

## How to reproduce it
You need a mesh with unused vertices. I use Capital Whiterun Expansion's `SurScafWalkwayNew.nif`: its shape `WRScafGuardTowerBase01:2` has 72 triangles and 236 vertices, and 80 of those vertices belong to no triangle.

1. `cow WhiterunWorld 5 -2`, set the time to noon, `tgm`.
2. Put decals on the walkway until one covers the triangles with the high vertex numbers. I cast Firebolt at it from eight points around it, 60 volleys ([repro/walkway.py](repro/walkway.py)). Scorch marks are the same kind of decal as footprints.

Without the fix every launch crashes, each after lookups past the end of the table. With it none crashes and no lookup leaves the table. The crash addresses vary from launch to launch, as expected for heap corruption.

## Where I met it
Walking through Whiterun: the direct form, RAX = 0x3617. The dump shows a footprint from a footprints mod landing on the walkway shape above.

A handful of older crashes of mine, all in Whiterun, show the same kind of heap damage. They most likely have the same cause; none of them was caught at the write.

Of the 81,525 meshes in my load order, 16 have shapes like this, three of them full-detail meshes. The problem is not limited to one mod.

## Why it crashes
When a decal lands on a shape, the game cuts the shape's triangles against the decal and collects the pieces. Many pieces share corners, so it keeps a table from "vertex number in the shape" to "vertex number in the decal". The first time it needs the table it allocates one, with room for `triangle count x 3` entries.

Then it indexes the table by the shape's vertex number.

In a clean mesh that works, because no vertex number can exceed three times the triangle count. A shape with unused vertices breaks the assumption. The walkway's table has 72 x 3 = 216 entries, and its triangles use vertex numbers up to 235. Lookups from 216 up read the next block on the heap as if it were the table.

What comes back is an invalid decal vertex number. In my crash, slot 220 returned 0x3617, and the next step read texture coordinates for vertex 0x3617 of a decal that can't have more than 512.

The next step also writes. It adds a tangent vector, three floats, to an array at that same index. The game reads the index as a signed 16-bit number, so the write lands up to 393 KB before or after the array. This is the heap damage: twelve bytes of some unrelated object get floats added to them. Without the fix, the crashes land on the bytes this step overwrote.

## The code, with the fix in
The fix changes one byte. This is the part of the function that sets the table up, decompiled with the patch in:

```c
// BSTempEffectSimpleDecal, SkyrimSE+4AB710 (Address Library 30112): add a clipped polygon to the decal
bool BSTempEffectSimpleDecal::AddPolygon(BSTempEffectSimpleDecal* decal, uint cornerCount, int* shapeVertex, float* positions, float* normals)
{
    uint16_t next = decal->vertexCount;                        // decal + 0xF8
    if (cornerCount + next >= 0x200) {
        return false;                                          // a decal holds at most 512 vertices
    }

    if (decal->positions == nullptr) {                         // first polygon: allocate the decal's buffers
        ...
        BSTriShape* shape = decal->target->vfunc_0x48();       // decal + 0x178: the shape the decal lands on
        size_t count = 0;
        if (shape != nullptr) {
            count = shape->vertexCount;                        // shape + 0x15A.  PATCHED, was:
                                                               // shape->triangleCount (shape + 0x158)
        }
        decal->remap = (uint16_t*)Allocate(count * 6);         // count * 3 entries of 2 bytes, decal + 0x150
        memset(decal->remap, 0xFF, count * 6);                 // 0xFFFF = "this vertex is not in the decal yet"
    }
```

One read feeds both the allocation and the fill, which is why one byte is enough. The loop that uses the table stays as it is:

```c
    for (uint c = 0; c < cornerCount; ++c) {
        int v = shapeVertex[c];                                // the corner's vertex number in the shape
        if (v == -1) {
            ...                                                // a corner the clipping created: matched by
                                                               // position against the corners made so far
        } else {
            int16_t known = decal->remap[v];                   // never bounds-checked. Safe now: v < vertexCount
            if (known == -1) {
                ...                                            // copy position, normal, colour into slot `next`
                decal->remap[v] = next;                        // remember it for the next polygon
                shapeVertex[c] = next;
                ++next;
            } else {
                shapeVertex[c] = known;                        // seen before: reuse the decal vertex
            }
        }
    }
    ...                                                        // emit the polygon as a triangle fan of these numbers
```

The byte itself: `movzx ecx, word ptr [rax+158h]` at `SkyrimSE+4AB853` becomes `[rax+15Ah]`. Install searches the start of the function for that instruction and expects exactly one, followed by the `lea` that triples the count. Anything else and it patches nothing.

## Why the fix looks like this
I fixed the size and left the readers alone. Every vertex number is below the vertex count, so a table sized by the vertex count can't be overrun, and a correct table never returns an invalid number. One byte stops the bad read, the float writes, and a third thing the code does: the lookup storing a decal number into memory past the table.

The table ends up three times larger than it needs to be, because the game's `x 3` stays. For the walkway that is 708 two-byte entries.

## What else I considered
Fixing the meshes was the first alternative: strip the unused vertices from the affected ones and ship them as a patch. Every update of those mods, and every new mod with such a mesh, would need it again.

A bounds check on the lookup would also stop the overrun, but it needs new code in the middle of the function, and a vertex past the table would have no table entry to record its decal vertex. The resize is one byte.

Scanning for such meshes after every mod install detects the problem without fixing it, and it has to be repeated after every install.
