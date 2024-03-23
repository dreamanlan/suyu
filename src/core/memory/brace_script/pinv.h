#pragma once

//modified from https://github.com/PX4/PX4-Autopilot/tree/4a3d64f1d76856d22323d1061ac6e560efda0a05/src/lib/matrix

#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <functional>

namespace matrix
{
    class Matrix;
    using LogCallback = std::function<void(const Matrix&, const char*)>;
    struct Helper
    {
        static LogCallback& LogRef()
        {
            static LogCallback s_Log{};
            return s_Log;
        }
        static inline void Log(const Matrix& m, const char* tag)
        {
            if (LogRef())
                LogRef()(m, tag);
        }
    };
    class Matrix
    {
    public:

        // Constructors
        Matrix() :m_Data(nullptr), m_M(0), m_N(0)
        {
        }

        Matrix(std::size_t m, std::size_t n)
        {
            m_Data = new double[m * n];
            m_M = m;
            m_N = n;
            setZero();
        }

        Matrix(const double data_[], std::size_t m, std::size_t n)
        {
            m_Data = new double[m * n];
            m_M = m;
            m_N = n;
            for (std::size_t i = 0; i < m; i++) {
                for (std::size_t j = 0; j < n; j++) {
                    m_Data[n * i + j] = data_[n * i + j];
                }
            }
        }

        Matrix(const Matrix& other)
        {
            std::size_t m = other.m_M;
            std::size_t n = other.m_N;
            m_Data = new double[m * n];
            m_M = m;
            m_N = n;
            for (std::size_t i = 0; i < m; i++) {
                for (std::size_t j = 0; j < n; j++) {
                    m_Data[n * i + j] = other(i, j);
                }
            }
        }

        virtual ~Matrix()
        {
            delete[] m_Data;
            m_Data = nullptr;
            m_M = 0;
            m_N = 0;
        }

        inline std::size_t GetM()const
        {
            return m_M;
        }

        inline std::size_t GetN()const
        {
            return m_N;
        }

        inline const double operator()(std::size_t i, std::size_t j) const
        {
            assert(i < m_M);
            assert(j < m_N);

            return m_Data[m_N * i + j];
        }

        inline double& operator()(std::size_t i, std::size_t j)
        {
            assert(i < m_M);
            assert(j < m_N);

            return m_Data[m_N * i + j];
        }

        inline void setZero()
        {
            setAll(0);
        }

        inline void setAll(double val)
        {
            Matrix& self = *this;
            for (std::size_t i = 0; i < m_M; i++) {
                for (std::size_t j = 0; j < m_N; j++) {
                    self(i, j) = val;
                }
            }
        }

        inline void setIdentity()
        {
            setZero();
            Matrix& self = *this;

            const std::size_t min_i = m_M > m_N ? m_N : m_M;
            for (std::size_t i = 0; i < min_i; i++) {
                self(i, i) = 1;
            }
        }

        inline void swapRows(std::size_t a, std::size_t b)
        {
            assert(a < m_M);
            assert(b < m_M);

            if (a == b) {
                return;
            }

            Matrix& self = *this;

            for (std::size_t j = 0; j < m_N; j++) {
                double tmp = self(a, j);
                self(a, j) = self(b, j);
                self(b, j) = tmp;
            }
        }

        inline void swapCols(std::size_t a, std::size_t b)
        {
            assert(a < m_N);
            assert(b < m_N);

            if (a == b) {
                return;
            }

            Matrix& self = *this;

            for (std::size_t i = 0; i < m_M; i++) {
                double tmp = self(i, a);
                self(i, a) = self(i, b);
                self(i, b) = tmp;
            }
        }

        Matrix& operator=(const Matrix& other)
        {
            if (this != &other) {
                Matrix& self = *this;
                if (m_M != other.m_M || m_N != other.m_N) {
                    if (m_Data) {
                        delete[] m_Data;
                    }
                    std::size_t m = other.m_M;
                    std::size_t n = other.m_N;
                    m_Data = new double[m * n];
                    m_M = m;
                    m_N = n;
                }
                for (std::size_t i = 0; i < m_M; i++) {
                    for (std::size_t j = 0; j < m_N; j++) {
                        self(i, j) = other(i, j);
                    }
                }
            }
            return (*this);
        }

        Matrix operator*(const Matrix& other) const
        {
            if (m_N == other.m_M) {
                const Matrix& self = *this;
                Matrix res(self.m_M, other.m_N);
                for (std::size_t i = 0; i < m_M; i++) {
                    for (std::size_t k = 0; k < other.m_N; k++) {
                        for (std::size_t j = 0; j < m_N; j++) {
                            res(i, k) += self(i, j) * other(j, k);
                        }
                    }
                }
                return res;
            }
            else {
                return Matrix{};
            }
        }

        Matrix transpose() const
        {
            Matrix res(m_N, m_M);
            const Matrix& self = *this;

            for (std::size_t i = 0; i < m_M; i++) {
                for (std::size_t j = 0; j < m_N; j++) {
                    res(j, i) = self(i, j);
                }
            }

            return res;
        }

        double max() const
        {
            double max_val = (*this)(0, 0);
            for (std::size_t i = 0; i < m_M; i++) {
                for (std::size_t j = 0; j < m_N; j++) {
                    double val = (*this)(i, j);
                    if (val > max_val) {
                        max_val = val;
                    }
                }
            }
            return max_val;
        }

        double min() const
        {
            double min_val = (*this)(0, 0);
            for (std::size_t i = 0; i < m_M; i++) {
                for (std::size_t j = 0; j < m_N; j++) {
                    double val = (*this)(i, j);
                    if (val < min_val) {
                        min_val = val;
                    }
                }
            }
            return min_val;
        }

    protected:
        double* m_Data;
        std::size_t m_M;
        std::size_t m_N;
    };

    class SquareMatrix final : public Matrix
    {
    public:
        SquareMatrix() :Matrix()
        {}

        SquareMatrix(std::size_t m) :Matrix(m, m)
        {}

        SquareMatrix(const double data_[], int m) :
            Matrix(data_, m, m)
        {
        }

        SquareMatrix(const Matrix &other) :
            Matrix(other)
        {
            assert(other.GetM() == other.GetN());
        }

        SquareMatrix& operator=(const Matrix&other)
        {
            assert(other.GetM() == other.GetN());
            Matrix::operator=(other);
            return *this;
        }

        double diagmax() const
        {
            double max_val = (*this)(0, 0);
            const SquareMatrix& self = *this;

            for (std::size_t i = 1; i < m_M; i++) {
                double v = self(i, i);
                if (v > max_val)
                    max_val = v;
            }
            return max_val;
        }
    };
    /**
     * inverse based on LU factorization with partial pivotting
     */
    bool inv(const SquareMatrix& A, SquareMatrix& inv, std::size_t rank)
    {
        int m = static_cast<int>(A.GetM());
        SquareMatrix L(m);
        L.setIdentity();
        SquareMatrix U = A;
        SquareMatrix P(m);
        P.setIdentity();

        // for all diagonal elements
        for (std::size_t n = 0; n < rank; n++) {

            // if diagonal is zero, swap with row below
            if (std::abs(U(n, n)) < DBL_EPSILON) {
                for (std::size_t i = n + 1; i < rank; i++) {

                    if (std::abs(U(i, n)) > DBL_EPSILON) {
                        U.swapRows(i, n);
                        P.swapRows(i, n);
                        L.swapRows(i, n);
                        L.swapCols(i, n);
                        break;
                    }
                }
            }

            // failsafe, return zero matrix
            if (std::abs(U(n, n)) < DBL_EPSILON) {
                return false;
            }

            // for all rows below diagonal
            for (std::size_t i = (n + 1); i < rank; i++) {
                L(i, n) = U(i, n) / U(n, n);

                // add i-th row and n-th row
                // multiplied by: -a(i,n)/a(n,n)
                for (std::size_t k = n; k < rank; k++) {
                    U(i, k) -= L(i, n) * U(n, k);
                }
            }
        }

        // for all columns of Y
        for (std::size_t c = 0; c < rank; c++) {
            // for all rows of L
            for (std::size_t i = 0; i < rank; i++) {
                // for all columns of L
                for (std::size_t j = 0; j < i; j++) {
                    // for all existing y
                    // subtract the component they
                    // contribute to the solution
                    P(i, c) -= L(i, j) * P(j, c);
                }
            }
        }

        // for all columns of X
        for (std::size_t c = 0; c < rank; c++) {
            // for all rows of U
            for (std::size_t k = 0; k < rank; k++) {
                // have to go in reverse order
                std::size_t i = rank - 1 - k;

                // for all columns of U
                for (std::size_t j = i + 1; j < rank; j++) {
                    // for all existing x
                    // subtract the component they
                    // contribute to the solution
                    P(i, c) -= U(i, j) * P(j, c);
                }

                // divide by the factor
                // on current
                // term to be solved
                //
                // we know that U(i, i) != 0 from above
                P(i, c) /= U(i, i);
            }
        }

        //check sanity of results
        for (std::size_t i = 0; i < rank; i++) {
            for (std::size_t j = 0; j < rank; j++) {
                if (!std::isfinite(P(i, j))) {
                    return false;
                }
            }
        }
        inv = P;
        return true;
    }

    SquareMatrix fullRankCholesky(const SquareMatrix& A, std::size_t& rank);
    /**
     * Geninv
     * Fast pseudoinverse based on full rank cholesky factorisation
     *
     * Courrieu, P. (2008). Fast Computation of Moore-Penrose Inverse Matrices, 8(2), 25–29. http://arxiv.org/abs/0804.4809
     */
    bool geninv(const Matrix& G, Matrix& res)
    {
        int m = static_cast<int>(G.GetM());
        int n = static_cast<int>(G.GetN());
        std::size_t rank;
        if (m <= n) {
            SquareMatrix A = G * G.transpose();
            SquareMatrix L = fullRankCholesky(A, rank);

            A = L.transpose() * L;
            SquareMatrix X;
            if (!inv(A, X, rank)) {
                res = Matrix{};
                return false; // LCOV_EXCL_LINE -- this can only be hit from numerical issues
            }
            // doing an intermediate assignment reduces stack usage
            A = X * X * L.transpose();
            res = G.transpose() * (L * A);

        }
        else {
            SquareMatrix A = G.transpose() * G;
            SquareMatrix L = fullRankCholesky(A, rank);

            A = L.transpose() * L;
            SquareMatrix X;
            if (!inv(A, X, rank)) {
                res = Matrix{};
                return false; // LCOV_EXCL_LINE -- this can only be hit from numerical issues
            }
            // doing an intermediate assignment reduces stack usage
            A = X * X * L.transpose();
            res = (L * A) * G.transpose();
        }
        return true;
    }

    /**
     * Full rank Cholesky factorization of A
     */
    SquareMatrix fullRankCholesky(const SquareMatrix& A, std::size_t& rank)
    {
        std::size_t n = A.GetN();
        // Loses one ulp accuracy per row of diag, relative to largest magnitude
        double tol = n * DBL_EPSILON * A.diagmax();

        Matrix L(n, n);

        std::size_t r = 0;
        for (std::size_t k = 0; k < n; k++) {

            if (r == 0) {
                for (std::size_t i = k; i < n; i++) {
                    L(i, r) = A(i, k);
                }

            }
            else {
                for (std::size_t i = k; i < n; i++) {
                    // Compute LL = L[k:n, :r] * L[k, :r].T
                    double LL = 0;
                    for (std::size_t j = 0; j < r; j++) {
                        LL += L(i, j) * L(k, j);
                    }
                    L(i, r) = A(i, k) - LL;
                }
            }
            if (L(k, r) > tol) {
                L(k, r) = sqrt(L(k, r));

                if (k < n - 1) {
                    for (std::size_t i = k + 1; i < n; i++) {
                        L(i, r) = L(i, r) / L(k, r);
                    }
                }

                r = r + 1;
            }
        }

        // Return rank
        rank = r;

        return L;
    }

} // namespace matrix
