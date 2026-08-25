#include "SkinCuboidGeometry.h"

#include <QVector2D>
#include <QVector3D>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

struct Vertex { float x,y,z,nx,ny,nz,u,v; };
struct Rect { float x0,y0,x1,y1; };
struct PartData { float width,height,depth; std::array<Rect,6> uv; };

PartData dataFor(const SkinCuboidGeometry::Part part)
{
    using P = SkinCuboidGeometry::Part;
    switch (part) {
    case P::Head: return {8,8,8, {{{0,8,8,16},{16,8,24,16},{8,0,16,8},
                                  {16,0,24,8},{8,8,16,16},{24,8,32,16}}}};
    case P::Body: return {8,12,4, {{{16,20,20,32},{28,20,32,32},{20,16,28,20},
                                    {28,16,36,20},{20,20,28,32},{32,20,40,32}}}};
    case P::RightArm: return {4,12,4, {{{40,20,44,32},{48,20,52,32},{44,16,48,20},
                                        {48,16,52,20},{44,20,48,32},{52,20,56,32}}}};
    case P::LeftArm: return {4,12,4, {{{32,52,36,64},{40,52,44,64},{36,48,40,52},
                                       {40,48,44,52},{36,52,40,64},{44,52,48,64}}}};
    case P::RightLeg: return {4,12,4, {{{0,20,4,32},{8,20,12,32},{4,16,8,20},
                                        {8,16,12,20},{4,20,8,32},{12,20,16,32}}}};
    case P::LeftLeg: return {4,12,4, {{{16,52,20,64},{24,52,28,64},{20,48,24,52},
                                       {24,48,28,52},{20,52,24,64},{28,52,32,64}}}};
    }
    return {};
}

} // namespace

SkinCuboidGeometry::SkinCuboidGeometry(QQuick3DObject *parent) : QQuick3DGeometry(parent)
{
    rebuild();
}

void SkinCuboidGeometry::setPart(const Part part)
{
    if (m_part == part) return;
    m_part = part;
    rebuild();
    emit partChanged();
}

void SkinCuboidGeometry::rebuild()
{
    clear();
    const PartData data = dataFor(m_part);
    const float x = data.width * 0.5F, y = data.height * 0.5F, z = data.depth * 0.5F;
    // right, left, top, bottom, front, back; four corners per face.
    const std::array<std::array<QVector3D, 4>, 6> positions{{
        {{QVector3D(x,-y,-z), QVector3D(x,-y,z), QVector3D(x,y,z), QVector3D(x,y,-z)}},
        {{QVector3D(-x,-y,z), QVector3D(-x,-y,-z), QVector3D(-x,y,-z), QVector3D(-x,y,z)}},
        {{QVector3D(-x,y,-z), QVector3D(x,y,-z), QVector3D(x,y,z), QVector3D(-x,y,z)}},
        {{QVector3D(-x,-y,z), QVector3D(x,-y,z), QVector3D(x,-y,-z), QVector3D(-x,-y,-z)}},
        {{QVector3D(-x,-y,z), QVector3D(-x,y,z), QVector3D(x,y,z), QVector3D(x,-y,z)}},
        {{QVector3D(x,-y,-z), QVector3D(x,y,-z), QVector3D(-x,y,-z), QVector3D(-x,-y,-z)}}
    }};
    const std::array<QVector3D, 6> normals{{
        QVector3D(1,0,0), QVector3D(-1,0,0), QVector3D(0,1,0),
        QVector3D(0,-1,0), QVector3D(0,0,1), QVector3D(0,0,-1)
    }};
    std::array<Vertex,24> vertices{};
    for (std::size_t face = 0; face < 6; ++face) {
        const Rect uv = data.uv[face];
        const std::array<QVector2D,4> coords{{{uv.x0/64,uv.y1/64},{uv.x0/64,uv.y0/64},
                                              {uv.x1/64,uv.y0/64},{uv.x1/64,uv.y1/64}}};
        for (std::size_t corner = 0; corner < 4; ++corner) {
            const QVector3D p = positions[face][corner];
            const QVector3D n = normals[face];
            const QVector2D t = coords[corner];
            vertices[face*4+corner] = {p.x(),p.y(),p.z(),n.x(),n.y(),n.z(),t.x(),t.y()};
        }
    }
    std::array<std::uint16_t,36> indices{};
    for (std::uint16_t face = 0; face < 6; ++face) {
        const std::uint16_t base = static_cast<std::uint16_t>(face * 4);
        const std::size_t out = static_cast<std::size_t>(face) * 6;
        indices[out+0]=base; indices[out+1]=base+1; indices[out+2]=base+2;
        indices[out+3]=base; indices[out+4]=base+2; indices[out+5]=base+3;
    }
    setStride(sizeof(Vertex));
    addAttribute(Attribute::PositionSemantic, offsetof(Vertex,x), Attribute::F32Type);
    addAttribute(Attribute::NormalSemantic, offsetof(Vertex,nx), Attribute::F32Type);
    addAttribute(Attribute::TexCoord0Semantic, offsetof(Vertex,u), Attribute::F32Type);
    addAttribute(Attribute::IndexSemantic, 0, Attribute::U16Type);
    setVertexData(QByteArray(reinterpret_cast<const char*>(vertices.data()), sizeof(vertices)));
    setIndexData(QByteArray(reinterpret_cast<const char*>(indices.data()), sizeof(indices)));
    setPrimitiveType(PrimitiveType::Triangles);
    setBounds(QVector3D(-x,-y,-z), QVector3D(x,y,z));
}
