#if !defined(_WIN32) && !defined(WIN32)
#error "This code is written for Windows!"
#endif

#if !defined(_WIN64)
#error "The code is written for OS 64bits!"
#endif

#if !defined(__AVX2__)
#error "Compiler does not support AVX2!"
#endif

#ifdef _DEBUG 
#error "The code is in DEBUG MODE"
#endif


#define HYPER_THREADING
#define CACHE_LINE_SIZE 64

#include <Windows.h>
#include <immintrin.h> // AVX2
#include <xmmintrin.h> // prefetch
#include <omp.h> //OpenMP
#include <thread>

using namespace std;

inline int **GenerateMatrixArray(int m, int n)
{
	int **matrix = new int*[m];
	register int temp_n = n;// (n + 7) & (~7); //makes n can be mod by 8(AVX-2 256bits)
	int *tmp = (int*)calloc(m * temp_n + CACHE_LINE_SIZE, sizeof(int)); //Add a CACHE_LINE_SIZE to Prevent False-Sharing

	for (register int i = 0; i < m; ++i)
		matrix[i] = &(tmp[temp_n * i]); //Assign the matrix
	return matrix;
}

inline void matrix_prefetch_one_time(register int *matrix)
{
	_mm_prefetch((char *)matrix, _MM_HINT_NTA);
}

int main(int argc, char* argv[]) {
	double t1 = omp_get_wtime(), t2, t3, _m_t2;

	unsigned int nthreads = std::thread::hardware_concurrency();
	HANDLE hProcess = GetCurrentProcess();
	SetPriorityClass(hProcess, REALTIME_PRIORITY_CLASS);// Set Process as Realtime
	FILE *input;
	FILE *output;
	int **matrix_1, **matrix_2;
	char **each_thread_data;
	long long int *each_thread_data_length;
	int m1, n1, m2, n2, node_count;

#pragma omp parallel num_threads(nthreads)
	{
		HANDLE hThread = GetCurrentThread();
		int threadId = omp_get_thread_num();
#ifdef HYPER_THREADING
		DWORD_PTR threadAffinityMask = 3 << (threadId  & (~1)); //2 Thread Share 2 Logical processor
#else
		DWORD_PTR threadAffinityMask = 1 << threadId; //Each Thread has its own processor
#endif
		SetThreadAffinityMask(hThread, threadAffinityMask);//Set Thread Affinity
		SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL); //Set Thread as Time_Critical

#pragma omp single
		{
			if (fopen_s(&input, (argc == 1) ? "input.txt" : argv[1], "r")) exit(1);
			fscanf_s(input, "%d%d", &m1, &n1);
			node_count = ((n1 + 7) & (~7)); // 8 element = one node
			matrix_1 = GenerateMatrixArray(m1, node_count);

			for (int i = 0; i < m1; ++i)
				for (int j = 0; j < n1; ++j)
					fscanf_s(input, "%d", &matrix_1[i][j]);

			fscanf_s(input, "%d%d", &m2, &n2);
			if (n1 != m2)
			{
				puts("Error! n1 should be m2!");
				exit(1);
			}

			matrix_2 = GenerateMatrixArray(n2, node_count);

			for (int i = 0; i < m2; ++i)
				for (int j = 0; j < n2; ++j)
					fscanf_s(input, "%d", &matrix_2[j][i]);

			fclose(input);
			t2 = omp_get_wtime();
		}

#pragma omp single
		{
			each_thread_data = new char*[nthreads];
		}

#pragma omp single
		{
			if (fopen_s(&output, "avx_output.txt", "w")) exit(1);
		}

#pragma omp single
		{
			each_thread_data_length = new long long int[nthreads];
		}

#pragma omp barrier
		int my_block_start = m1 * threadId / nthreads;
		matrix_prefetch_one_time(matrix_2[0]);
		matrix_prefetch_one_time(matrix_1[my_block_start]);
		int my_block_end = ((threadId + 1) == nthreads) ? m1 : m1 * (threadId + 1) / nthreads;
		int my_block_size = my_block_end - my_block_start;
		int **ResultMatrix = GenerateMatrixArray(m1, n2);
		matrix_prefetch_one_time(ResultMatrix[my_block_start]);
		for (register int i = my_block_start; i < my_block_end; ++i)
		{
			for (register int j = 0; true; )
			{
				register __m256i result = _mm256_setzero_si256(); // result is zero

				for (register int k = 0; k < node_count; k+= 8)
				{
					register __m256i *left  = (__m256i*)&matrix_1[i][k];
					register __m256i *right = (__m256i*)&matrix_2[j][k];
					register __m256i temp = _mm256_mullo_epi32(*left, *right);
					result = _mm256_add_epi32(result, temp);
				}
				{
					register long long int *temp = (long long int*)&result;
					register long long int Val = temp[0] + temp[1]
						+ temp[2] + temp[3];
					ResultMatrix[i][j] = (int)Val + (Val >> 32);
				}

				if (++j == m2) //For Branch hint
				{
					matrix_prefetch_one_time(matrix_2[0]);	//prefetch next loop data
					break;
				}
				else
					continue;

			}
			//No need to flush, let hardware handle because a row data may smaller than cache line
		}
		if (omp_get_thread_num() == 0) _m_t2 = omp_get_wtime();
		matrix_prefetch_one_time(ResultMatrix[my_block_start]);
		char *stream_output = (char *)malloc(my_block_size * n2 * sizeof("-2147483648 ") + my_block_size * sizeof("\n"));
		char *stream_output_ptr = stream_output;
		for (register int i = my_block_start; i < my_block_end; ++i)
		{
			for (register int j = 0; j < n2; ++j)
				stream_output_ptr += snprintf(stream_output_ptr, sizeof("-2147483648 "), "%d ", ResultMatrix[i][j]);
			stream_output_ptr[0] = '\n';
			stream_output_ptr += 1;
		}
		if (my_block_start == my_block_end) 
		{
			each_thread_data[threadId] = 0;
			each_thread_data_length[threadId] = 0;
		}
		else
		{
			each_thread_data[threadId] = stream_output; //Do at here, reduce false sharing effect
			each_thread_data_length[threadId] = stream_output_ptr - stream_output;
		}

#pragma omp barrier

#pragma omp master
		{
			for (register unsigned int i = 0; i < nthreads; ++i) 
			{
				if (each_thread_data[i] == 0) continue;
				if (i + 1 != nthreads && each_thread_data[i + 1] != 0)
					matrix_prefetch_one_time((int*)each_thread_data[i + 1]); // 4 * Cache Line
				fwrite(each_thread_data[i], each_thread_data_length[i], 1, output);
			}
		}
#pragma omp barrier //Protect Local Variable not to be clean
	}
	t3 = omp_get_wtime();
	fclose(output);
	FILE *performance;
	if (fopen_s(&performance, "Performance_AVX.txt", "w")) exit(1);
	char performance_output[64];
	fwrite(performance_output, snprintf(performance_output, 64, "Total: %.3f msec\n", (t3 - t1) * 1000), 1, performance);
	fwrite(performance_output, snprintf(performance_output, 64, "ReadF: %.3f msec\n", (t2 - t1) * 1000), 1, performance);
	fwrite(performance_output, snprintf(performance_output, 64, "Compu: %.3f msec (Compute only)\n", (_m_t2 - t2) * 1000), 1, performance);
	fwrite(performance_output, snprintf(performance_output, 64, "Compu: %.3f msec (I/O only)\n", (t3 - _m_t2) * 1000), 1, performance);
	fwrite(performance_output, snprintf(performance_output, 64, "Compu: %.3f msec (All part)\n", (t3 - t2) * 1000), 1, performance);
	fclose(performance);
	Sleep(1000);
	return 0;
}