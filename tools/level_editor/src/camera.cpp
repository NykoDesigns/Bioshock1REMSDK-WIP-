#include "camera.h"
#include <cstring>

static constexpr float DEG2RAD = 3.14159265f / 180.0f;

void Camera::Orbit(float dx, float dy)
{
    yaw += dx * 0.3f;
    pitch += dy * 0.3f;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
}

void Camera::Pan(float dx, float dy)
{
    float yr = yaw * DEG2RAD;
    // Right vector
    float rx = cosf(yr);
    float ry = -sinf(yr);
    // Up is always Z
    float speed = distance * 0.001f;
    target.x += rx * dx * speed + 0 * dy * speed;
    target.y += ry * dx * speed + 0 * dy * speed;
    target.z += dy * speed;
}

void Camera::Zoom(float delta)
{
    distance *= (1.0f - delta * 0.1f);
    if (distance < 100.0f) distance = 100.0f;
    if (distance > 200000.0f) distance = 200000.0f;
}

void Camera::FocusOn(Vec3 pos)
{
    target = pos;
    distance = 3000.0f;
}

void Camera::BeginFly()
{
    if (!flyMode) {
        m_FlyPos = GetPosition();
        flyMode = true;
    }
}

void Camera::EndFly()
{
    if (flyMode) {
        flyMode = false;
        // Sync orbit target to where we're looking
        Vec3 fwd = GetForward();
        target.x = m_FlyPos.x + fwd.x * distance;
        target.y = m_FlyPos.y + fwd.y * distance;
        target.z = m_FlyPos.z + fwd.z * distance;
    }
}

void Camera::FlyMove(float forward, float right, float up, float dt)
{
    if (!flyMode) return;
    Vec3 fwd = GetForward();
    Vec3 rt = GetRight();
    float speed = flySpeed * dt;
    m_FlyPos.x += (fwd.x * forward + rt.x * right) * speed;
    m_FlyPos.y += (fwd.y * forward + rt.y * right) * speed;
    m_FlyPos.z += (fwd.z * forward + rt.z * right + up) * speed;
}

void Camera::FlyLook(float dx, float dy)
{
    yaw += dx * 0.15f;
    pitch -= dy * 0.15f;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
}

Vec3 Camera::GetPosition() const
{
    if (flyMode) return m_FlyPos;
    float yr = yaw * DEG2RAD;
    float pr = pitch * DEG2RAD;
    Vec3 pos;
    pos.x = target.x + distance * cosf(pr) * cosf(yr);
    pos.y = target.y + distance * cosf(pr) * sinf(yr);
    pos.z = target.z + distance * sinf(pr);
    return pos;
}

Vec3 Camera::GetForward() const
{
    float yr = yaw * DEG2RAD;
    float pr = pitch * DEG2RAD;
    return {-cosf(pr) * cosf(yr), -cosf(pr) * sinf(yr), -sinf(pr)};
}

Vec3 Camera::GetRight() const
{
    float yr = yaw * DEG2RAD;
    return {-sinf(yr), cosf(yr), 0};
}

Mat4 Camera::GetViewMatrix() const
{
    Vec3 pos = GetPosition();
    if (flyMode) {
        Vec3 fwd = GetForward();
        Vec3 lookAt = {pos.x + fwd.x, pos.y + fwd.y, pos.z + fwd.z};
        return LookAt(pos, lookAt, {0, 0, 1});
    }
    return LookAt(pos, target, {0, 0, 1});
}

Mat4 Camera::GetProjectionMatrix(float aspect) const
{
    return Perspective(fov, aspect, nearPlane, farPlane);
}

Mat4 Camera::LookAt(Vec3 eye, Vec3 center, Vec3 up)
{
    Vec3 f = {center.x - eye.x, center.y - eye.y, center.z - eye.z};
    float flen = sqrtf(f.x*f.x + f.y*f.y + f.z*f.z);
    f.x /= flen; f.y /= flen; f.z /= flen;

    Vec3 s = {f.y*up.z - f.z*up.y, f.z*up.x - f.x*up.z, f.x*up.y - f.y*up.x};
    float slen = sqrtf(s.x*s.x + s.y*s.y + s.z*s.z);
    s.x /= slen; s.y /= slen; s.z /= slen;

    Vec3 u = {s.y*f.z - s.z*f.y, s.z*f.x - s.x*f.z, s.x*f.y - s.y*f.x};

    Mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = s.x;  m.m[4] = s.y;  m.m[8]  = s.z;  m.m[12] = -(s.x*eye.x + s.y*eye.y + s.z*eye.z);
    m.m[1] = u.x;  m.m[5] = u.y;  m.m[9]  = u.z;  m.m[13] = -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z; m.m[14] = (f.x*eye.x + f.y*eye.y + f.z*eye.z);
    m.m[3] = 0;    m.m[7] = 0;    m.m[11] = 0;    m.m[15] = 1;
    return m;
}

Mat4 Camera::Perspective(float fovDeg, float aspect, float nearP, float farP)
{
    float tanHalf = tanf(fovDeg * 0.5f * DEG2RAD);
    Mat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = 1.0f / (aspect * tanHalf);
    m.m[5] = 1.0f / tanHalf;
    m.m[10] = -(farP + nearP) / (farP - nearP);
    m.m[11] = -1.0f;
    m.m[14] = -(2.0f * farP * nearP) / (farP - nearP);
    return m;
}
