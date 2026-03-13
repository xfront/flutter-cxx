#pragma once
/// @file foundation/matrix4.h
/// @brief 4×4 列主序矩阵，对应 上游的 `vector_math.Matrix4`。
///
/// 与 上游的一致性要点：
///   - 存储顺序与 上游 相同：**列主序**（`m_[col * 4 + row]`），
///     即 `at(row, col) == m_[col * 4 + row]`。
///   - `multiply(other)` 的语义与 上游 一致：`this = this * other`
///     （上游的 `Matrix4.multiply` 也是右乘）。
///   - `translate` / `scale` / `rotateZ` 这些便捷方法同样是右乘（等价于
///     上游的 `Matrix4.translate` 等就地变换）。
///
/// 只覆盖 2D 渲染真正需要的部分（平移 / 缩放 / 绕 Z 旋转 / 斜切），
/// 第三维保留以便将来接 3D 变换。

#include <cmath>

#include "cui/foundation/geometry.h"

namespace cui {

class Matrix4 {
public:
    Matrix4() { reset(); }

    // ── 工厂 ──────────────────────────────────────────────────────────────

    static Matrix4 identity() {
        Matrix4 m;
        return m;
    }

    /// 平移矩阵。上游的 `Matrix4.translationValues`。
    static Matrix4 translation(double x, double y, double z = 0.0) {
        Matrix4 m;
        m.m_[12] = x;
        m.m_[13] = y;
        m.m_[14] = z;
        return m;
    }

    /// 缩放矩阵。上游的 `Matrix4.diagonal3Values`。
    static Matrix4 scale(double x, double y = 1.0, double z = 1.0) {
        Matrix4 m;
        m.m_[0] = x;
        m.m_[5] = y;
        m.m_[10] = z;
        return m;
    }

    /// 绕 Z 轴旋转（弧度）。上游的 `Matrix4.rotationZ`。
    static Matrix4 rotationZ(double radians) {
        Matrix4 m;
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        m.m_[0] = c;
        m.m_[1] = s;
        m.m_[4] = -s;
        m.m_[5] = c;
        return m;
    }

    /// 沿 X 方向斜切（tan 值）。上游的 `Matrix4.skewX`。
    static Matrix4 skewX(double alpha) {
        Matrix4 m;
        m.m_[4] = alpha;
        return m;
    }

    /// 沿 Y 方向斜切（tan 值）。上游的 `Matrix4.skewY`。
    static Matrix4 skewY(double beta) {
        Matrix4 m;
        m.m_[1] = beta;
        return m;
    }

    // ── 元素访问 ──────────────────────────────────────────────────────────

    /// `(row, col)` 处的元素（row / col ∈ [0, 3]）。
    double at(int row, int col) const { return m_[col * 4 + row]; }
    void set(int row, int col, double v) { m_[col * 4 + row] = v; }

    /// 底层列主序存储（上游的 `Matrix4.storage`）。
    const double* storage() const { return m_; }

    // ── 就地变换（均为右乘，语义同上游）────────────────────────────

    /// `this = this * other`。
    void multiply(const Matrix4& other) {
        double out[16] = {};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) {
                double sum = 0.0;
                for (int k = 0; k < 4; ++k)
                    sum += m_[k * 4 + r] * other.m_[c * 4 + k];
                out[c * 4 + r] = sum;
            }
        for (int i = 0; i < 16; ++i)
            m_[i] = out[i];
    }

    void translate(double x, double y, double z = 0.0) { multiply(Matrix4::translation(x, y, z)); }
    void scaleBy(double x, double y = 1.0, double z = 1.0) { multiply(Matrix4::scale(x, y, z)); }
    void rotateZ(double radians) { multiply(Matrix4::rotationZ(radians)); }
    void skewXBy(double alpha) { multiply(Matrix4::skewX(alpha)); }
    void skewYBy(double beta) { multiply(Matrix4::skewY(beta)); }

    /// 重置为单位矩阵。
    void setIdentity() { reset(); }

    // ── 查询 / 运算 ───────────────────────────────────────────────────────

    bool isIdentity() const {
        for (int i = 0; i < 16; ++i)
            if (m_[i] != kIdentity[i])
                return false;
        return true;
    }

    /// 变换一个 2D 点（z 取 0），含齐次除法。
    Offset transformOffset(const Offset& o) const {
        const double x = o.dx;
        const double y = o.dy;
        const double w = m_[3] * x + m_[7] * y + m_[15];
        const double nx = m_[0] * x + m_[4] * y + m_[12];
        const double ny = m_[1] * x + m_[5] * y + m_[13];
        if (w == 0.0 || w == 1.0)
            return {nx, ny};
        return {nx / w, ny / w};
    }

    /// 逆矩阵。仅处理仿射部分（最后一行视为 [0 0 0 1]），奇异时返回单位矩阵。
    Matrix4 inverse() const {
        // 3×3 线性部分（行主序读取）。
        const double a = at(0, 0), b = at(0, 1), c = at(0, 2);
        const double d = at(1, 0), e = at(1, 1), f = at(1, 2);
        const double g = at(2, 0), h = at(2, 1), i = at(2, 2);
        const double A = e * i - f * h;
        const double B = -(d * i - f * g);
        const double C = d * h - e * g;
        const double det = a * A + b * B + c * C;
        if (std::abs(det) < 1e-12)
            return Matrix4::identity();

        const double invDet = 1.0 / det;
        // 伴随矩阵 / det（转置后的余子式矩阵）。
        const double r00 = A * invDet;
        const double r01 = -(b * i - c * h) * invDet;
        const double r02 = (b * f - c * e) * invDet;
        const double r10 = B * invDet;
        const double r11 = (a * i - c * g) * invDet;
        const double r12 = -(a * f - c * d) * invDet;
        const double r20 = C * invDet;
        const double r21 = -(a * h - b * g) * invDet;
        const double r22 = (a * e - b * d) * invDet;

        const double tx = at(0, 3), ty = at(1, 3), tz = at(2, 3);
        Matrix4 out;
        out.set(0, 0, r00);
        out.set(0, 1, r01);
        out.set(0, 2, r02);
        out.set(1, 0, r10);
        out.set(1, 1, r11);
        out.set(1, 2, r12);
        out.set(2, 0, r20);
        out.set(2, 1, r21);
        out.set(2, 2, r22);
        // 平移部分取 -R⁻¹ · t。
        out.set(0, 3, -(r00 * tx + r01 * ty + r02 * tz));
        out.set(1, 3, -(r10 * tx + r11 * ty + r12 * tz));
        out.set(2, 3, -(r20 * tx + r21 * ty + r22 * tz));
        return out;
    }

    bool operator==(const Matrix4& o) const {
        for (int i = 0; i < 16; ++i)
            if (m_[i] != o.m_[i])
                return false;
        return true;
    }
    bool operator!=(const Matrix4& o) const { return !(*this == o); }

private:
    /// 就地重置为单位矩阵（`identity()` 是工厂方法，名字留给 上游语义）。
    void reset() {
        for (int i = 0; i < 16; ++i)
            m_[i] = kIdentity[i];
    }

    static constexpr double kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    double m_[16] = {};
};

/// `a * b`（上游的 `Matrix4.multiply` 的纯函数版本）。
inline Matrix4 matrix4Multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 r = a;
    r.multiply(b);
    return r;
}

}  // namespace cui_core
