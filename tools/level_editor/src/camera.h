#pragma once

#include <cmath>

struct Vec3 { float x, y, z; };
struct Mat4 { float m[16]; };

// Camera with orbit + FPS fly mode
class Camera {
public:
    Vec3 target = {0, 0, 0};
    float distance = 5000.0f;
    float yaw = 45.0f;    // degrees
    float pitch = 30.0f;  // degrees
    float fov = 60.0f;
    float nearPlane = 10.0f;
    float farPlane = 500000.0f;
    float flySpeed = 5000.0f; // units/sec in fly mode
    bool flyMode = false;     // true when RMB held (FPS mode)

    // Input
    void Orbit(float dx, float dy);
    void Pan(float dx, float dy);
    void Zoom(float delta);
    void FocusOn(Vec3 pos);

    // FPS fly mode
    void BeginFly();  // switch to fly mode (RMB pressed)
    void EndFly();    // switch back to orbit (RMB released)
    void FlyMove(float forward, float right, float up, float dt);
    void FlyLook(float dx, float dy);

    // Matrices
    Vec3 GetPosition() const;
    Vec3 GetForward() const;
    Vec3 GetRight() const;
    Mat4 GetViewMatrix() const;
    Mat4 GetProjectionMatrix(float aspect) const;

    // Utility
    static Mat4 LookAt(Vec3 eye, Vec3 center, Vec3 up);
    static Mat4 Perspective(float fovDeg, float aspect, float near, float far);

private:
    Vec3 m_FlyPos = {0, 0, 0}; // camera position in fly mode
};
