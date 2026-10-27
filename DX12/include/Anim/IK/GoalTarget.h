#pragma once
#include "Anim/IK/Kinematics.h"
namespace Anim::IK {
inline DirectX::XMMATRIX GoalFrame(DirectX::FXMMATRIX source,DirectX::FXMVECTOR offset,
                                 DirectX::FXMVECTOR rotation) {
    return DirectX::XMMatrixRotationQuaternion(rotation)*DirectX::XMMatrixTranslationFromVector(offset)*source;
}
}
