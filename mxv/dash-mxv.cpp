#include <unistd.h>
#include <iostream>
#include <cstddef>
#include <iomanip>
#include <chrono>

#include <libdash.h>

#if defined(HAVE_MKL_CBLAS)
#include <mkl_cblas.h>
#elif defined(HAVE_CBLAS)
#include <cblas.h>
#endif

template<class MatrixT>
void print_matrix(const MatrixT & matrix)
{
  typedef typename MatrixT::value_type value_t;
  auto& pattern = matrix.pattern();
  auto rows = matrix.extent(0);
  auto cols = matrix.extent(1);

  // Creating local copy for output to prevent interleaving with log
  // messages:
  value_t * matrix_copy = new value_t[matrix.size()];
  auto copy_end = std::copy(matrix.begin(),
                            matrix.end(),
                            matrix_copy);
  DASH_ASSERT(copy_end == matrix_copy + matrix.size());
  std::string first = "[";
  for (auto r = 0; r < rows; ++r) {
    std::cout << first;
    for (auto c = 0; c < cols; ++c) {
      //std::cout << " " << std::setw(5) << matrix_copy[r * cols + c] << " # @" << pattern.unit_at({r,c}) << "\n";
      std::cout << " " << std::setw(5) << matrix_copy[r * cols + c];
    }
    std::cout << std::endl;
    first = ";";
  }
  std::cout << "];" << std::endl;
  delete[] matrix_copy;
}

template<class VectorT>
void print_vector(const VectorT & vector)
{
    auto size = vector.size();

    std::cout << "[";
    for (auto i = 0; i < size; ++i) {
        std::cout << " " << std::setw(5) << (double)vector[i];
    }
    std::cout << "\n];" << std::endl;
}



template<typename T>
void product(T* y, const T* A, const T* x, int M, int N)
{
    for (int m = 0; m < M; m++) {
        T sum = (T)0;
        for (int n = 0; n < N; n++) {
            sum += A[m * N + n] * x[n];
        }
        y[m] += sum;
    }
}

#if defined(HAVE_CBLAS) || defined(MKL_HAVE_CBLAS)
template<>
void product<double>(double* y, const double* A, const double* x, int M, int N)
{
    cblas_dgemv(CblasRowMajor, CblasNoTrans,
                M, N,
                1.0, A, M,
                x, 1, 1.0,
                y, 1);
}

template<>
void product<float>(float* y, const float* A, const float* x, int M, int N)
{
    cblas_sgemv(CblasRowMajor, CblasNoTrans,
                M, N,
                1.0, A, M,
                x, 1, 1.0,
                y, 1);
}
#endif

template<typename>
struct is_tile_pattern : std::false_type {};

template<
  dash::dim_t      NumDimensions,
  dash::MemArrange Arrangement,
  typename         IndexType>
struct is_tile_pattern<dash::TilePattern<NumDimensions, Arrangement, IndexType>> : std::true_type {};

template<typename Data>
void product_tile_pattern(const dash::Matrix<Data,2>& A,
                          const dash::Array<Data>&    x,
                          dash::Array<Data>&          y)
{
    if (A.size() <= 1024 && dash::myid() == 0) {
        std::cout << A.pattern().blockspec() << std::endl;
        std::cout << A.pattern().local_blockspec() << std::endl;
        std::cout << A.pattern().distspec() << std::endl;
        std::cout << A.pattern().sizespec() << std::endl;

        std::cout << "product using "
                  << A.extent(0) << "x" <<  A.extent(1)
                  << " (" << A.local.extent(0) << " x " << A.local.extent(1) << ")"
                  << " matrix " << std::endl;
    }
    auto& pattern = A.pattern();
    static_assert(is_tile_pattern<typename dash::Matrix<Data,2>::pattern_type>::value,
                  "This works only for TilePattern.");

    // local copy of x
    std::vector<Data> local_x(x.size());
    dash::copy(x.begin(), x.end(), local_x.data());
    // local result space for y
    std::vector<Data> local_y(y.size(), 0.0);

    auto lblocks = pattern.local_blockspec().size();
    for (size_t lblock_idx = 0; lblock_idx < lblocks; lblock_idx++ ) {
        auto lblock_view = pattern.local_block_local(lblock_idx);
        auto local_index = pattern.local_at({0,0}, lblock_view);

        auto global_index = pattern.global(local_index);
        auto global_coords = pattern.coords(global_index);
        //std::cout << dash::myid() << ": " << lblock_idx << " -> " << lblock_view << " : {0,0} -> " << local_index << " -> " << global_index << " -> {" << global_coords[0] << "," << global_coords[1] << "}" << std::endl;

        /* begin of the local block */
        auto *lblock_begin = A.lbegin() + local_index;

        /* begin of the local x */
        auto x_begin = local_x.data() + global_coords[1];

        /* begin of the local y */
        auto y_begin = local_y.data() + global_coords[0];

        auto M = lblock_view.extent(0);
        auto N = lblock_view.extent(1);
        product(y_begin, lblock_begin, x_begin, M, N);
    }

    /* reduce local result vectors into global y vector */
    if (A.size() <= 1024) {
        std::cout << dash::myid() << ": local Vector y size: " << local_y.size() << std::endl;
        print_vector(local_y);
    }

    auto& team = y.team();
    auto team_size = team.size();
    team.barrier();

    dash::NArray<Data,2> result_matrix(
        dash::SizeSpec<2>(
            y.size(),
            team_size),
       dash::DistributionSpec<2>{},
        team);
    auto& result_pattern = result_matrix.pattern();

    auto myid = team.myid();
    for (decltype(local_y.size()) i = 0; i < local_y.size(); ++i) {
        result_matrix[i][myid] = local_y[i];
    }
    team.barrier();

    for (auto r = 0; r < y.size(); ++r) {
        const auto& coord = result_pattern.local({r,0});
        if (coord.unit != myid)
            continue;

        Data sum = (Data)0;
        for (decltype(team_size) c = 0; c < team_size; ++c) {
            sum += result_matrix.local[coord.coords[0]][c];
        }
        y[r] = sum;
    }
    team.barrier();
}

int main(int argc, char* argv[])
{
    dash::init(&argc, &argv);

    dart_unit_t myid = dash::myid();
    size_t num_units = dash::Team::All().size();

    dash::TeamSpec<2> teamspec_2d(num_units, 1);
    teamspec_2d.balance_extents();

    size_t size_factor = 4;
    if (argc > 1) {
        std::istringstream in(argv[1]);
        in >> size_factor;
    }
    size_t tile_size  = 4;
    if (argc > 2) {
        std::istringstream in(argv[2]);
        in >> tile_size;
    }
    size_t rows = tile_size * teamspec_2d.num_units(0) * size_factor;
    size_t cols = tile_size * teamspec_2d.num_units(1) * size_factor;
    size_t matrix_size = rows * cols;

    if (matrix_size <= 1024 && 0 == myid) {
        std::cout << "Matrix size: " << rows
                  << " x " << cols
                  << " == " << matrix_size
                  << std::endl;
    }

    dash::Matrix<double, 2> matrix(
                         dash::SizeSpec<2>(
                           rows,
                           cols),
                         dash::DistributionSpec<2>(
                           dash::TILE(tile_size),
                           dash::TILE(tile_size)),
                         dash::Team::All(),
                         teamspec_2d);
    DASH_ASSERT(matrix_size == matrix.size());
    DASH_ASSERT(rows == matrix.extent(0));
    DASH_ASSERT(cols == matrix.extent(1));

    dash::Array<double> vector_x(cols);
    dash::Array<double> vector_y(rows);

    dash::Team::All().barrier();

    std::fill(matrix.lbegin(), matrix.lend(), (double)myid);
    std::fill(vector_x.lbegin(), vector_x.lend(), (double)myid);
    std::fill(vector_y.lbegin(), vector_y.lend(), 0.0);

    dash::Team::All().barrier();
    if (matrix_size <= 1024 && 0 == myid) {
        print_matrix(matrix);

        std::cout << "Vector x size: " << vector_x.size() << std::endl;
        print_vector(vector_x);
    }

    auto const tpStart(std::chrono::high_resolution_clock::now());
    dash::Team::All().barrier();

    product_tile_pattern(matrix, vector_x, vector_y);

    dash::Team::All().barrier();
    auto const tpEnd(std::chrono::high_resolution_clock::now());

    auto const durElapsed(tpEnd - tpStart);
    if (0 == myid) {
        std::cout << rows << " " << cols << " " << std::chrono::duration_cast<std::chrono::microseconds>(durElapsed).count() << std::endl;
    }

    if (matrix_size <= 1024 && 0 == myid) {
        std::cout << "Vector y size: " << vector_y.size() << std::endl;
        print_vector(vector_y);
    }

    dash::Team::All().barrier();

    dash::finalize();

    return 0;
}
