#pragma once

#include <QtQuick3D/QQuick3DGeometry>

class SkinCuboidGeometry : public QQuick3DGeometry
{
    Q_OBJECT
    Q_PROPERTY(Part part READ part WRITE setPart NOTIFY partChanged)

public:
    enum class Part { Head, Body, RightArm, LeftArm, RightLeg, LeftLeg };
    Q_ENUM(Part)

    explicit SkinCuboidGeometry(QQuick3DObject *parent = nullptr);
    [[nodiscard]] Part part() const noexcept { return m_part; }
    void setPart(Part part);

signals:
    void partChanged();

private:
    void rebuild();
    Part m_part = Part::Head;
};
