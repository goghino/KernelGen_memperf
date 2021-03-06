// TODO parameterize shadow boundaries thicknesses

//===----------------------------------------------------------------------===//
//
//     KernelGen -- A prototype of LLVM-based auto-parallelizing Fortran/C
//        compiler for NVIDIA GPUs, targeting numerical modeling code.
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include "timing.h"

#if defined(__CUDACC__)
    #include "nvToolsExt.h"
    #include "cuda_profiling.h"
    #include <cuda.h>
    #include <curand_kernel.h>
#endif

//default stencil code, regular pageable host explicit memory copies, stencil kernel running on device
#define DEFAULT 0
//UVM memory(no explicit memcopies), stencil kernel running on host
#define UVM_HOST 1
//UVM memory(no explicit memcopies), stencil kernel running on device
#define UVM_DEVICE 2
//host uses pinned memory, explicit memory copies, stencil kernel running on device
#define PINNED 3
//host uses pinned memory, device uses mapped host memory(no explicit memcopies), stencil kernel running on device
#define MAPPED 4

//specifies where data are generated/initialized
#define HOST_INIT 1
#define DEVICE_INIT 2

// Memory alignment, for vectorization on MIC.
// 4096 should be best for memory transfers over PCI-E.
#define MEMALIGN 4096

#define _A(array, is, iy, ix) (array[(ix) + nx * (iy) + nx * ny * (is)])

/**
    Host version_k
*/
void wave13pt_h(const int nx, const int ny, const int ns,
	const real m0, const real m1, const real m2,
	const real* const __restrict__ w0p, const real* const __restrict__ w1p,
	real* const __restrict__ w2p)
{
	const real* const __restrict__ w0 = w0p;
	const real* const __restrict__ w1 = w1p;
	real* const __restrict__ w2 = w2p;

	int i_stride = 1;
	int j_stride = 1;
	int k_stride = 1;
	int k_offset = 0;
	int j_offset = 0;
	int i_offset = 0;

	int k_increment = k_stride;
	int j_increment = j_stride;
	int i_increment = i_stride;

#if defined(_OPENMP)
    //fprintf(stderr, "OPENMP version_k\n");
	#pragma omp parallel for
#endif
	for (int k = 2 + k_offset; k < ns - 2; k += k_increment)
	{
		for (int j = 2 + j_offset; j < ny - 2; j += j_increment)
		{
			for (int i = i_offset; i < nx; i += i_increment)
			{
				if ((i < 2) || (i >= nx - 2)) continue;

				_A(w2, k, j, i) =  m0 * _A(w1, k, j, i) - _A(w0, k, j, i) +

					m1 * (
						_A(w1, k, j, i+1) + _A(w1, k, j, i-1)  +
						_A(w1, k, j+1, i) + _A(w1, k, j-1, i)  +
						_A(w1, k+1, j, i) + _A(w1, k-1, j, i)) +
					m2 * (
						_A(w1, k, j, i+2) + _A(w1, k, j, i-2)  +
						_A(w1, k, j+2, i) + _A(w1, k, j-2, i)  +
						_A(w1, k+2, j, i) + _A(w1, k-2, j, i));
			}
		}
	}
}

/**
    Device version_k
*/
#if defined(__CUDACC__)
extern "C" __global__
void wave13pt_d(const int nx, const int ny, const int ns,
	kernel_config_t config,
	const real m0, const real m1, const real m2,
	const real* const __restrict__ w0p, const real* const __restrict__ w1p,
	real* const __restrict__ w2p)
{
	const real* const __restrict__ w0 = w0p;
	const real* const __restrict__ w1 = w1p;
	real* const __restrict__ w2 = w2p;


	int i_stride = (config.strideDim.x);
	int j_stride = (config.strideDim.y);
	int k_stride = (config.strideDim.z);
	int k_offset = (blockIdx.z * blockDim.z + threadIdx.z);
	int j_offset = (blockIdx.y * blockDim.y + threadIdx.y);
	int i_offset = (blockIdx.x * blockDim.x + threadIdx.x);

	int k_increment = k_stride;
	int j_increment = j_stride;
	int i_increment = i_stride;

	for (int k = 2 + k_offset; k < ns - 2; k += k_increment)
	{
		for (int j = 2 + j_offset; j < ny - 2; j += j_increment)
		{
			for (int i = i_offset; i < nx; i += i_increment)
			{
				if ((i < 2) || (i >= nx - 2)) continue;

				_A(w2, k, j, i) =  m0 * _A(w1, k, j, i) - _A(w0, k, j, i) +

					m1 * (
						_A(w1, k, j, i+1) + _A(w1, k, j, i-1)  +
						_A(w1, k, j+1, i) + _A(w1, k, j-1, i)  +
						_A(w1, k+1, j, i) + _A(w1, k-1, j, i)) +
					m2 * (
						_A(w1, k, j, i+2) + _A(w1, k, j, i-2)  +
						_A(w1, k, j+2, i) + _A(w1, k, j-2, i)  +
						_A(w1, k+2, j, i) + _A(w1, k-2, j, i));
			}
		}
	}
}

/**
    Init cuda PRNG generator by seed, seed equal to id
*/
extern "C" __global__ void init_curand(curandState *state, 
                         const int nx, const int ny, const int ns,
                         kernel_config_t config)
{
	int k = (blockIdx.z * blockDim.z + threadIdx.z);
	int j = (blockIdx.y * blockDim.y + threadIdx.y);
	int i = (blockIdx.x * blockDim.x + threadIdx.x);

	int id = (i) + nx * (j) + nx * ny * (k);
    curand_init(1337, id, 0, &state[id]);

}
/**
    Init grids w0, w1, w2 by random numbers
*/
extern "C" __global__ void init_grid_d(real* w0, real* w1, real* w2, curandState *state,
                                        const int nx, const int ny, const int ns,
                                        kernel_config_t config)
{
	int i_stride = (config.strideDim.x);
	int j_stride = (config.strideDim.y);
	int k_stride = (config.strideDim.z);
	int k_offset = (blockIdx.z * blockDim.z + threadIdx.z);
	int j_offset = (blockIdx.y * blockDim.y + threadIdx.y);
	int i_offset = (blockIdx.x * blockDim.x + threadIdx.x);

	int k_increment = k_stride;
	int j_increment = j_stride;
	int i_increment = i_stride;

    int id = (i_offset) + nx * (j_offset) + nx * ny * (k_offset);

	for (int k = k_offset; k < ns ; k += k_increment)
	{
		for (int j = j_offset; j < ny ; j += j_increment)
		{
			for (int i = i_offset; i < nx; i += i_increment)
			{
                w0[id] = (curand_uniform(&state[id]) - 0.5)/2;
                w1[id] = (curand_uniform(&state[id]) - 0.5)/2;
                w2[id] = (curand_uniform(&state[id]) - 0.5)/2;
			}
		}
	} 
}

/**
    Init grids w0, w1, w2 by constant numbers
*/
extern "C" __global__ void init_grid_d_const(real* w0, real* w1, real* w2,
                                        const int nx, const int ny, const int ns,
                                        kernel_config_t config)
{
	int i_stride = (config.strideDim.x);
	int j_stride = (config.strideDim.y);
	int k_stride = (config.strideDim.z);
	int k_offset = (blockIdx.z * blockDim.z + threadIdx.z);
	int j_offset = (blockIdx.y * blockDim.y + threadIdx.y);
	int i_offset = (blockIdx.x * blockDim.x + threadIdx.x);

	int k_increment = k_stride;
	int j_increment = j_stride;
	int i_increment = i_stride;

	for (int k = k_offset; k < ns ; k += k_increment)
	{
		for (int j = j_offset; j < ny ; j += j_increment)
		{
			for (int i = i_offset; i < nx; i += i_increment)
			{
                _A(w0, k, j, i) = 0.1;
                _A(w1, k, j, i) = 0.1;
                _A(w2, k, j, i) = 0.1;
			}
		}
	} 
}
#endif

#define parse_arg(name, arg) \
	int name = atoi(arg); \
	if (name < 0) \
	{ \
		printf("Value for " #name " is invalid: %d\n", name); \
		exit(1); \
	}

#define real_rand() (((real)(rand() / (double)RAND_MAX) - 0.5) * 2)

// init stencil grid by random data
#define init_grid_h(a, b, c) \
	for (int i = 0; i < szarray; i++) \
	{ \
		a[i] = real_rand(); \
		b[i] = real_rand(); \
		c[i] = real_rand(); \
		mean += a[i] + b[i] + c[i]; \
	} \
	printf("initial mean = %f\n", mean / szarray / 3);

// final mean
#define f_mean(arr) \
        mean = 0.0f; \
	    for (int i = 0; i < szarray; i++) \
		    mean += arr[i]; \
	    printf("final mean = %f\n", mean / szarray);

int main(int argc, char* argv[])
{
#if defined(__CUDACC__)
	nvtxRangePushA("parse_args");
#endif
	if (argc != 7)
	{
		printf("Usage: %s <nx> <ny> <ns> <nt> <version_k> <version_i>\n", argv[0]);
		exit(1);
	}

	const char* no_timing = getenv("NO_TIMING");

#if defined(__CUDACC__)
	char* regcount_fname = getenv("PROFILING_FNAME");
	if (regcount_fname)
	{
		char* regcount_lineno = getenv("PROFILING_LINENO");
		int lineno = -1;
		if (regcount_lineno)
			lineno = atoi(regcount_lineno);
		kernel_enable_regcount(regcount_fname, lineno);
	}
#endif

	parse_arg(nx, argv[1]);
	parse_arg(ny, argv[2]);
	parse_arg(ns, argv[3]);
	parse_arg(nt, argv[4]);
	parse_arg(version_k, argv[5]);//where kernel is run, what host memory is used
    parse_arg(version_i, argv[6]);//where data are generated/initialized

	real m0 = real_rand();  //random stencil coeficients
	real m1 = real_rand() / 6.;
	real m2 = real_rand() / 6.;

	printf("m0 = %f, m1 = %f, m2 = %f\n", m0, m1, m2);
#if defined(__CUDACC__)
	nvtxRangePop();
#endif

	size_t szarray = (size_t)nx * ny * ns;
	size_t szarrayb = szarray * sizeof(real);

    //grids for stencil computation    
    real* w0;
    real* w1;
    real* w2;

    real mean = 0.0f;

#if defined(__CUDACC__)
    nvtxRangePushA("host_allocation");
#endif
    if(version_k == DEFAULT){//allocate memory on host	
        w0 = (real*)memalign(MEMALIGN, szarrayb);
	    w1 = (real*)memalign(MEMALIGN, szarrayb);
	    w2 = (real*)memalign(MEMALIGN, szarrayb);

	    if (!w0 || !w1 || !w2)
	    {
		    printf("Error allocating memory for arrays: %p, %p, %p\n", w0, w1, w2);
		    exit(1);
	    }
    }

#if defined(__CUDACC__)
    if (version_k == PINNED) {
        CUDA_SAFE_CALL(cudaHostAlloc(&w0,szarrayb,cudaHostAllocDefault));
        CUDA_SAFE_CALL(cudaHostAlloc(&w1,szarrayb,cudaHostAllocDefault));
        CUDA_SAFE_CALL(cudaHostAlloc(&w2,szarrayb,cudaHostAllocDefault));
    }else if (version_k == MAPPED) {
        CUDA_SAFE_CALL(cudaHostAlloc(&w0,szarrayb,cudaHostAllocMapped));
        CUDA_SAFE_CALL(cudaHostAlloc(&w1,szarrayb,cudaHostAllocMapped));
        CUDA_SAFE_CALL(cudaHostAlloc(&w2,szarrayb,cudaHostAllocMapped));
    }
    nvtxRangePop();
#endif

	//
	// 1) Perform an empty offload, that should strip
	// the initialization time from further offloads.
	//  
#if defined(__CUDACC__)
    nvtxRangePushA("empty_offload");
	volatile struct timespec init_s, init_f;
	get_time(&init_s);
	int count = 0;
	CUDA_SAFE_CALL(cudaGetDeviceCount(&count));
	get_time(&init_f);
	double init_t = get_time_diff((struct timespec*)&init_s, (struct timespec*)&init_f);
	if (!no_timing) printf("init time = %f sec\n", init_t);
    nvtxRangePop();
#endif
    

	//
	// 2) Allocate data on device, but do not copy anything.
	//
#if defined(__CUDACC__)
    real *w0_dev = NULL, *w1_dev = NULL, *w2_dev = NULL;
    nvtxRangePushA("cuda_alloc");
	volatile struct timespec alloc_s, alloc_f;
	get_time(&alloc_s);
    if(version_k == DEFAULT || version_k == PINNED) {//explicit memory model
	    CUDA_SAFE_CALL(cudaMalloc(&w0_dev, szarrayb));
	    CUDA_SAFE_CALL(cudaMalloc(&w1_dev, szarrayb));
	    CUDA_SAFE_CALL(cudaMalloc(&w2_dev, szarrayb));
    }else if (version_k == MAPPED) {
        CUDA_SAFE_CALL(cudaHostGetDevicePointer(&w0_dev,w0,0));
        CUDA_SAFE_CALL(cudaHostGetDevicePointer(&w1_dev,w1,0));
        CUDA_SAFE_CALL(cudaHostGetDevicePointer(&w2_dev,w2,0));
    }
    else if(version_k == UVM_HOST || version_k == UVM_DEVICE){//UVM memory model
	    CUDA_SAFE_CALL(cudaMallocManaged(&w0, szarrayb)); //data are not yet initialized
	    CUDA_SAFE_CALL(cudaMallocManaged(&w1, szarrayb));
	    CUDA_SAFE_CALL(cudaMallocManaged(&w2, szarrayb));
    }
	get_time(&alloc_f);
	double alloc_t = get_time_diff((struct timespec*)&alloc_s, (struct timespec*)&alloc_f);
	if (!no_timing) printf("device buffer alloc time = %f sec\n", alloc_t);
    nvtxRangePop();
#endif

#if defined(__CUDACC__)
	CUDA_SAFE_CALL(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));
	dim3 gridDim, blockDim, strideDim;
	kernel_config_t config;
	kernel_configure_gird(1, nx, ny, ns, &config);
#endif

#if defined(__CUDACC__)
    nvtxRangePushA("grid_init");
#endif
    if(version_i == HOST_INIT)
    {
        //host init
        init_grid_h(w0, w1, w2);
    }else if(version_i == DEVICE_INIT && (version_k==UVM_HOST || version_k==UVM_DEVICE)){
        //device init
        
        /*
        curandState *state;
        CUDA_SAFE_CALL(cudaMalloc(&state, nx*ny*ns*sizeof(curandState)));
        init_curand<<<config.gridDim, config.blockDim>>>(state, nx, ny, ns, config);
	    CUDA_SAFE_CALL(cudaGetLastError());
	    //CUDA_SAFE_CALL(cudaDeviceSynchronize());
        init_grid_d<<<config.gridDim, config.blockDim>>>(w0,w1,w2,state,nx,ny,ns,config);
	    CUDA_SAFE_CALL(cudaGetLastError());
	    //CUDA_SAFE_CALL(cudaDeviceSynchronize());
        cudaFree(state);
        */
#if defined(__CUDACC__)
        init_grid_d_const<<<config.gridDim, config.blockDim>>>(w0,w1,w2,nx,ny,ns,config);
	    CUDA_SAFE_CALL(cudaGetLastError());
	    CUDA_SAFE_CALL(cudaDeviceSynchronize());
#endif
    }else{
        fprintf(stderr,"Invalid initialization option, initializing grids on host.\n");
        init_grid_h(w0, w1, w2);
    }
#if defined(__CUDACC__)
    nvtxRangePop();
#endif

	//
	// 3) Transfer data from host to device and leave it there,
	// i.e. do not allocate deivce memory buffers.
	//
#if defined(__CUDACC__)
    nvtxRangePushA("cuda_memcpy_explicit");
	volatile struct timespec load_s, load_f;
	get_time(&load_s);
    if(version_k == DEFAULT || version_k == PINNED){
	    CUDA_SAFE_CALL(cudaMemcpy(w0_dev, w0, szarrayb, cudaMemcpyHostToDevice));
	    CUDA_SAFE_CALL(cudaMemcpy(w1_dev, w1, szarrayb, cudaMemcpyHostToDevice));
	    CUDA_SAFE_CALL(cudaMemcpy(w2_dev, w2, szarrayb, cudaMemcpyHostToDevice));
    }
	get_time(&load_f);
	double load_t = get_time_diff((struct timespec*)&load_s, (struct timespec*)&load_f);
	if (!no_timing) printf("data load time = %f sec (%f GB/sec)\n", load_t, 3 * szarrayb / (load_t * 1024 * 1024 * 1024));
    nvtxRangePop();
#endif

	//
	// 4) Perform data processing iterations, keeping all data
	// on device.
	//
#if defined(__CUDACC__)
    nvtxRangePushA("kernel");	
#endif
    int idxs[] = { 0, 1, 2 };
	volatile struct timespec compute_s, compute_f;
	
    real *w0p;
    real *w1p;
    real *w2p;
#if !defined(__CUDACC__)
		w0p = w0, w1p = w1, w2p = w2;
#else
        if(version_k == DEFAULT || version_k == PINNED || version_k == MAPPED){
		    w0p = w0_dev, w1p = w1_dev, w2p = w2_dev;
        }else{ /*|| version_k == MAPPED*/
            w0p = w0, w1p = w1, w2p = w2;
        }
#endif
    get_time(&compute_s);
    {
		for (int it = 0; it < nt; it++)
		{
#if !defined(__CUDACC__)
			wave13pt_h(nx, ny, ns, m0, m1, m2, w0p, w1p, w2p);
#else
            if(version_k == UVM_HOST){
                nvtxRangePushA("core_h");
                wave13pt_h(nx, ny, ns, m0, m1, m2, w0p, w1p, w2p);
                nvtxRangePop();
            } else { // kernel running on DEVICE
                nvtxRangePushA("core_d");
			    wave13pt_d<<<config.gridDim, config.blockDim, config.szshmem>>>(
				    nx, ny, ns,
				    config,
				    m0, m1, m2, w0p, w1p, w2p);
			    CUDA_SAFE_CALL(cudaGetLastError());
			    CUDA_SAFE_CALL(cudaDeviceSynchronize());
                nvtxRangePop();
            }
#endif
			real* w = w0p; w0p = w1p; w1p = w2p; w2p = w;
			int idx = idxs[0]; idxs[0] = idxs[1]; idxs[1] = idxs[2]; idxs[2] = idx;
		}
	}
	get_time(&compute_f);
	double compute_t = get_time_diff((struct timespec*)&compute_s, (struct timespec*)&compute_f);
	if (!no_timing) printf("compute time = %f sec\n", compute_t);

#if !defined(__CUDACC__)
	real* w[] = { w0, w1, w2 }; 
	w0 = w[idxs[0]]; w1 = w[idxs[1]]; w2 = w[idxs[2]];

    //final mean
    f_mean(w1);
#else
    if(version_k == DEFAULT || version_k == PINNED ){
	    real* w_local[] = { w0_dev, w1_dev, w2_dev }; 
	    w0_dev = w_local[idxs[0]];
        w1_dev = w_local[idxs[1]];
        w2_dev = w_local[idxs[2]];
    }else{
        real* w_local[] = { w0, w1, w2 }; 
    	w0 = w_local[idxs[0]];
        w1 = w_local[idxs[1]];
        w2 = w_local[idxs[2]];
    }
#endif

#if defined(__CUDACC__)
    nvtxRangePop();
#endif

	//
	// 5) Transfer output data back from device to host.
	//   
#if defined(__CUDACC__)
    nvtxRangePushA("copy_back");
	volatile struct timespec save_s, save_f;
	get_time(&save_s);
    if(version_k == DEFAULT || version_k == PINNED) {
    	CUDA_SAFE_CALL(cudaMemcpy(w1, w1_dev, szarrayb, cudaMemcpyDeviceToHost));
    }
	get_time(&save_f);
	double save_t = get_time_diff((struct timespec*)&save_s, (struct timespec*)&save_f);
	if (!no_timing) printf("data save time = %f sec (%f GB/sec)\n", save_t, szarrayb / (save_t * 1024 * 1024 * 1024));
    nvtxRangePop();
#endif
    

	//
	// 6) Deallocate device data buffers.
	// OPENACC does not seem to have explicit deallocation.
	//
#if defined(_OPENACC)
	}
#endif
#if defined(__CUDACC__)
    // For the final mean - account only the norm of the top
	// most level (tracked by swapping idxs array of indexes).
    nvtxRangePushA("final_mean");    
    //volatile struct timespec save_s, save_f;
    get_time(&save_s);

    f_mean(w1);

    get_time(&save_f);
    save_t = get_time_diff((struct timespec*)&save_s, (struct timespec*)&save_f);
    if (!no_timing) printf("final mean time = %f sec\n", save_t);
    nvtxRangePop();

    nvtxRangePushA("free");
    volatile struct timespec free_s, free_f;
    get_time(&free_s);
    if(version_k == DEFAULT || version_k == PINNED){
	    CUDA_SAFE_CALL(cudaFree(w0_dev)); // free device memory
	    CUDA_SAFE_CALL(cudaFree(w1_dev));
	    CUDA_SAFE_CALL(cudaFree(w2_dev));
    }
    if (version_k == UVM_HOST || version_k == UVM_DEVICE){
	    CUDA_SAFE_CALL(cudaFree(w0));//free UVM memory 
	    CUDA_SAFE_CALL(cudaFree(w1));
	    CUDA_SAFE_CALL(cudaFree(w2));
    }
    if(version_k == MAPPED || version_k == PINNED){
        CUDA_SAFE_CALL(cudaFreeHost(w0));//free shared pinned-mapped host memory or pinned host memory
	    CUDA_SAFE_CALL(cudaFreeHost(w1));
	    CUDA_SAFE_CALL(cudaFreeHost(w2));
    }
	get_time(&free_f);
	double free_t = get_time_diff((struct timespec*)&free_s, (struct timespec*)&free_f);
	if (!no_timing) printf("device buffer free time = %f sec\n", free_t);
    nvtxRangePop();
#endif

    if(version_k == DEFAULT){

        //free host memory
	    free(w0);
	    free(w1);
	    free(w2);
    }
    

	fflush(stdout);
    

	return 0;
}

//TODO
//grid initialization by device does not work, kernel gets stalled at init_curand<<<...>>>()
