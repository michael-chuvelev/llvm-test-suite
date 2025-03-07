//==---------------- kmeans.cpp  - DPC++ ESIMD on-device test --------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// REQUIRES: gpu
// UNSUPPORTED: cuda
// RUN: %clangxx-esimd -fsycl %s -I%S/.. -o %t.out
// RUN: %HOST_RUN_PLACEHOLDER %t.out %S/points.csv
// RUN: %GPU_RUN_PLACEHOLDER %t.out %S/points.csv
//

#include "kmeans.h"
#include "esimd_test_utils.hpp"

#include <CL/sycl.hpp>
#include <CL/sycl/INTEL/esimd.hpp>
#include <fstream>
#include <iostream>
#include <string.h>
#include <vector>

using namespace cl::sycl;
using namespace sycl::INTEL::gpu;
using namespace std;

inline float dist(Point p, Centroid c) {
  float dx = p.x - c.x;
  float dy = p.y - c.y;
  return dx * dx + dy * dy;
}

void clustering(Point *pts,         // points
                unsigned num_pts,   // number of points
                Centroid *ctrds,    // centroids
                unsigned num_ctrds) // number of centroids
{

  for (auto i = 0; i < num_pts; i++) {
    float min_dis = -1;
    auto cluster_idx = 0;
    // for each point, compute the min distance to centroids
    for (auto j = 0; j < num_ctrds; j++) {
      float dis = dist(pts[i], ctrds[j]);

      if (dis < min_dis || min_dis == -1) {
        min_dis = dis;
        cluster_idx = j;
      }
    }
    pts[i].cluster = cluster_idx;
  }
  // compute new positions of centroids
  Accum *accum = (Accum *)malloc(num_ctrds * sizeof(Accum));
  memset(accum, 0, num_ctrds * sizeof(Accum));
  for (auto i = 0; i < num_pts; i++) {
    auto c = pts[i].cluster;
    accum[c].x_sum += pts[i].x;
    accum[c].y_sum += pts[i].y;
    accum[c].num_points++;
  }
  for (auto j = 0; j < num_ctrds; j++) {
    ctrds[j].x = accum[j].x_sum / accum[j].num_points;
    ctrds[j].y = accum[j].y_sum / accum[j].num_points;
    ctrds[j].num_points = accum[j].num_points;
  }
  delete accum;
}

#define max(a, b) (((a) > (b)) ? (a) : (b))

bool verify_result(Centroid4 *centroids4, // gpu centroids result
                   Centroid *centroids)   // cpu centroids result
{
  bool succ = true;
  int k = 0;
  int j = 0;
  for (auto i = 0; i < NUM_CENTROIDS_ACTUAL; i++) {
    float errX = fabs(centroids4[j].x[k] - centroids[i].x) /
                 max(fabs(centroids4[j].x[k]), fabs(centroids[i].x));
    float errY = fabs(centroids4[j].y[k] - centroids[i].y) /
                 max(fabs(centroids4[j].y[k]), fabs(centroids[i].y));
    float errSize =
        abs(centroids4[j].num_points[k] - centroids[i].num_points) /
        max(abs(centroids4[j].num_points[k]), abs(centroids[i].num_points));
    // std::cout << i << ": Wanted (" << centroids[i].x << ", " <<
    // centroids[i].y
    //        << ", " << centroids[i].num_points << ")" << std::endl;
    // std::cout << "Got (" << centroids4[j].x[k] << ", " << centroids4[j].y[k]
    //        << ", " << centroids4[j].num_points[k] << ")" << std::endl;
    if (errX >= 0.002f || errY >= 0.002f || errSize >= 0.002f) {
      std::cout << "Error, index " << i << ": Wanted (" << centroids[i].x
                << ", " << centroids[i].y << ", " << centroids[i].num_points
                << ")" << std::endl;
      std::cout << "Got (" << centroids4[j].x[k] << ", " << centroids4[j].y[k]
                << ", " << centroids4[j].num_points[k] << ")" << std::endl;
      succ = false;
      break;
    }
    k++;
    if (k == SIMD_SIZE) {
      k = 0;
      j++;
    }
  }

  return succ;
}

// take initial points and run k mean clustering number of iterations
void cpu_kmeans(Point *pts,          // points
                unsigned num_pts,    // number of points
                Centroid *ctrds,     // centroids
                unsigned num_ctrds,  // number of centroids
                unsigned iterations) // run clustering number of iterations
{
  for (auto i = 0; i < iterations; i++) {
    clustering(pts, num_pts, ctrds, num_ctrds);
  }
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: kmeans.exe input_file" << std::endl;
    exit(1);
  }

  cl::sycl::property_list props{property::queue::enable_profiling{},
                                property::queue::in_order()};
  queue q(esimd_test::ESIMDSelector{}, esimd_test::createExceptionHandler(),
          props);

  auto dev = q.get_device();
  auto ctxt = q.get_context();

  auto points4 = malloc_shared<Point4>(NUM_POINTS / SIMD_SIZE, q);
  memset(points4, 0, NUM_POINTS / SIMD_SIZE * sizeof(Point4));
  auto centroids4 =
      malloc_shared<Centroid4>(NUM_CENTROIDS_ALLOCATED / SIMD_SIZE, q);
  memset(centroids4, 0,
         NUM_CENTROIDS_ALLOCATED / SIMD_SIZE * sizeof(Centroid4));
  auto accum4 = malloc_shared<Accum4>(NUM_CENTROIDS_ALLOCATED, q);

  auto points = malloc_shared<Point>(NUM_POINTS, q);
  memset(points, 0, NUM_POINTS * sizeof(Point));
  auto centroids = malloc_shared<Centroid>(NUM_CENTROIDS_ACTUAL, q);
  memset(centroids, 0, NUM_CENTROIDS_ACTUAL * sizeof(Centroid));

  std::ifstream infile(argv[1]);
  if (!infile.is_open()) {
    std::cout << "Failed to open file " << argv[1] << std::endl;
    exit(-1);
  }

  std::string first, second;
  int i = 0;
  int j = 0;
  int k = 0;
  while (std::getline(infile, first, ',')) {
    if (i == NUM_POINTS)
      break;
    float x = std::stof(first);
    std::getline(infile, second, '\n');
    float y = std::stof(second);
    // std::cout << "( "<< x<<", "<< y << " )"<<std::endl;
    points[i].x = x;
    points[i].y = y;
    i++;
    points4[j].x[k] = x;
    points4[j].y[k] = y;
    k++;
    if (k == SIMD_SIZE) {
      k = 0;
      j++;
    }
  }
  infile.close();

  std::cout << "read in points" << std::endl;

  // init centroids with the first num_centroids points
  for (auto i = 0; i < NUM_CENTROIDS_ACTUAL; i++) {
    centroids[i].x = points[i].x;
    centroids[i].y = points[i].y;
    centroids[i].num_points = 0;
  }
  // compute CPU kmean results for verifying results later
  std::cout << "compute reference output" << std::endl;
  cpu_kmeans(points, NUM_POINTS, centroids, NUM_CENTROIDS_ACTUAL,
             NUM_ITERATIONS);

  std::cout << "compute reference output successful" << std::endl;

  memset(centroids4, 0,
         sizeof(Centroid4) * NUM_CENTROIDS_ALLOCATED / SIMD_SIZE);
  for (int i = 0; i < NUM_POINTS / SIMD_SIZE; i++) {
    for (int j = 0; j < SIMD_SIZE; j++) {
      points4[i].cluster[j] = 0;
    }
  }
  for (int i = 0; i < NUM_CENTROIDS_ACTUAL / SIMD_SIZE; i++) {
    for (int j = 0; j < SIMD_SIZE; j++) {
      centroids4[i].x[j] = points4[i].x[j];
      centroids4[i].y[j] = points4[i].y[j];
      centroids4[i].num_points[j] = 0;
    }
  }
  for (int j = 0; j < (NUM_CENTROIDS_ACTUAL & (SIMD_SIZE - 1)); j++) {
    centroids4[NUM_CENTROIDS_ACTUAL / SIMD_SIZE].x[j] =
        points4[NUM_CENTROIDS_ACTUAL / SIMD_SIZE].x[j];
    centroids4[NUM_CENTROIDS_ACTUAL / SIMD_SIZE].y[j] =
        points4[NUM_CENTROIDS_ACTUAL / SIMD_SIZE].y[j];
    centroids4[NUM_CENTROIDS_ACTUAL / SIMD_SIZE].num_points[j] = 0;
  }

  Point4 *kpoints4 = points4;
  Centroid4 *kcentroids4 = centroids4;
  Accum4 *kaccum4 = accum4;

  double kernel1_time_in_ms = 0;
  double kernel2_time_in_ms = 0;

  //----
  // Actual execution goes here

  unsigned int total_threads = (NUM_POINTS - 1) / POINTS_PER_THREAD + 1;
  auto GlobalRange = cl::sycl::range<1>(total_threads);
  cl::sycl::range<1> LocalRange{1};
  auto GlobalRange1 = cl::sycl::range<1>(NUM_CENTROIDS_ACTUAL);
  cl::sycl::range<1> LocalRange1 = cl::sycl::range<1>{1};

  auto submitJobs = [&]() {
    // kmeans
    auto e = q.submit([&](cl::sycl::handler &cgh) {
      cgh.parallel_for<class kMeans>(
          GlobalRange * LocalRange, [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
            simd<float, 2 * NUM_CENTROIDS_ALLOCATED> centroids(0);
            auto centroidsXYXY =
                centroids.format<float, NUM_CENTROIDS_ALLOCATED / SIMD_SIZE,
                                 SIMD_SIZE * 2>();
            auto centroidsXY =
                centroids.format<float, 2 * NUM_CENTROIDS_ALLOCATED / SIMD_SIZE,
                                 SIMD_SIZE>();

#pragma unroll
            for (int i = 0; i < NUM_CENTROIDS_ALLOCATED / SIMD_SIZE; i++) {
              centroidsXYXY.row(i) =
                  block_load<float, 2 * SIMD_SIZE>(kcentroids4[i].xyn);
            }

            simd<float, NUM_CENTROIDS_ALLOCATED> accumxsum(0);
            simd<float, NUM_CENTROIDS_ALLOCATED> accumysum(0);
            simd<int, NUM_CENTROIDS_ALLOCATED> accumnpoints(0);

            auto xsum =
                accumxsum.format<float, NUM_CENTROIDS_ALLOCATED / SIMD_SIZE,
                                 SIMD_SIZE>();
            auto ysum =
                accumysum.format<float, NUM_CENTROIDS_ALLOCATED / SIMD_SIZE,
                                 SIMD_SIZE>();
            auto npoints =
                accumnpoints.format<int, NUM_CENTROIDS_ALLOCATED / SIMD_SIZE,
                                    SIMD_SIZE>();

            // each thread handles POINTS_PER_THREAD points
            int index = it.get_global_id(0) * POINTS_PER_THREAD / SIMD_SIZE;

            for (int i = 0; i < POINTS_PER_THREAD / SIMD_SIZE; i++) {
              simd<float, 2 * SIMD_SIZE> points;
              auto pointsXY = points.format<float, 2, SIMD_SIZE>();
              simd<int, SIMD_SIZE> cluster(0);

              points =
                  block_load<float, 2 * SIMD_SIZE>(kpoints4[index + i].xyn);

              simd<float, SIMD_SIZE> dx =
                  pointsXY.row(0) - centroidsXY.row(0)[0];
              simd<float, SIMD_SIZE> dy =
                  pointsXY.row(1) - centroidsXY.row(1)[0];
              simd<float, SIMD_SIZE> min_dist = dx * dx + dy * dy;

#pragma unroll
              for (int j = 1; j < SIMD_SIZE; j++) {
                dx = pointsXY.row(0) - centroidsXY.row(0)[j];
                dy = pointsXY.row(1) - centroidsXY.row(1)[j];
                simd<float, SIMD_SIZE> dist = dx * dx + dy * dy;
                cluster.merge(j, (dist < min_dist));
                min_dist.merge(dist, (dist < min_dist));
              }

#pragma unroll
              for (int j = 1; j < NUM_CENTROIDS_ACTUAL / SIMD_SIZE; j++) {
                for (int k = 0; k < SIMD_SIZE; k++) {
                  // compute distance
                  dx = pointsXY.row(0) - (centroidsXY.row(2 * j + 0))[k];
                  dy = pointsXY.row(1) - (centroidsXY.row(2 * j + 1))[k];
                  simd<float, SIMD_SIZE> dist = dx * dx + dy * dy;
                  cluster.merge(j * SIMD_SIZE + k, (dist < min_dist));
                  min_dist.merge(dist, (dist < min_dist));
                }
              }
#pragma unroll
              for (int k = 0, j = (NUM_CENTROIDS_ACTUAL / SIMD_SIZE);
                   k < (NUM_CENTROIDS_ACTUAL & (SIMD_SIZE - 1)); k++) {
                // compute distance
                dx = pointsXY.row(0) - (centroidsXY.row(2 * j + 0))[k];
                dy = pointsXY.row(1) - (centroidsXY.row(2 * j + 1))[k];
                simd<float, SIMD_SIZE> dist = dx * dx + dy * dy;
                cluster.merge(j * SIMD_SIZE + k, (dist < min_dist));
                min_dist.merge(dist, (dist < min_dist));
              }

              block_store<int, SIMD_SIZE>(kpoints4[index + i].cluster, cluster);

#pragma unroll
              for (int k = 0; k < SIMD_SIZE; k++) {
                int c = cluster[k];
                int j = c / SIMD_SIZE;
                int m = c & (SIMD_SIZE - 1);

                xsum.row(j).select<1, 0>(m) += pointsXY.row(0)[k];
                ysum.row(j).select<1, 0>(m) += pointsXY.row(1)[k];
                npoints.row(j).select<1, 0>(m) += 1;
              }
            }
            simd<unsigned int, SIMD_SIZE> offsets(0, sizeof(Accum4));
            offsets += it.get_global_id(0) * sizeof(float);
#pragma unroll
            for (int i = 0, j = 0; i < NUM_CENTROIDS_ALLOCATED;
                 i += SIMD_SIZE, j++) {
              scatter<float, SIMD_SIZE>(kaccum4[i].x_sum, xsum.row(j), offsets);
              scatter<float, SIMD_SIZE>(kaccum4[i].y_sum, ysum.row(j), offsets);
              scatter<int, SIMD_SIZE>(kaccum4[i].num_points, npoints.row(j),
                                      offsets);
            }
          });
    });

    e.wait();
    kernel1_time_in_ms += esimd_test::report_time("kernel1", e, e);

    // printf("Done with kmeans\n");

    // compute centroid position
    auto e2 = q.submit([&](cl::sycl::handler &cgh) {
      cgh.parallel_for<class kCompCentroidPos>(
          GlobalRange1 * LocalRange1, [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
            simd<float, SIMD_SIZE> xsum(0);
            simd<float, SIMD_SIZE> ysum(0);
            simd<int, SIMD_SIZE> npoints(0);

            unsigned int offset = 0;
            for (int i = 0; i < (NUM_POINTS / POINTS_PER_THREAD) / SIMD_SIZE;
                 i++) {
              simd<float, SIMD_SIZE> t = block_load<float, SIMD_SIZE>(
                  kaccum4[it.get_global_id(0)].x_sum + offset);
              xsum += t;
              t = block_load<float, SIMD_SIZE>(
                  kaccum4[it.get_global_id(0)].y_sum + offset);
              ysum += t;
              simd<int, SIMD_SIZE> n = block_load<int, SIMD_SIZE>(
                  kaccum4[it.get_global_id(0)].num_points + offset);
              npoints += n;
              offset += SIMD_SIZE;
            }

            simd<float, SIMD_SIZE> centroid(0);
            int num = reduce<int>(npoints, std::plus<>());
            centroid.select<1, 0>(0) = reduce<float>(xsum, std::plus<>()) / num;
            centroid.select<1, 0>(1) = reduce<float>(ysum, std::plus<>()) / num;
            (centroid.format<int>()).select<1, 0>(2) = num;

            simd<ushort, SIMD_SIZE> mask(0);
            mask.select<3, 1>(0) = 1;
            int i = it.get_global_id(0) / SIMD_SIZE;
            int k = it.get_global_id(0) & (SIMD_SIZE - 1);
            simd<unsigned int, SIMD_SIZE> offsets(k * sizeof(float),
                                                  SIMD_SIZE * sizeof(float));
            scatter<float, SIMD_SIZE>(kcentroids4[i].xyn, centroid, offsets,
                                      mask);
          });
    });
    e2.wait();
    kernel2_time_in_ms += esimd_test::report_time("kernel2", e2, e2);
  };

  try {
    for (auto i = 0; i < NUM_ITERATIONS; i++) {
      submitJobs();
    }
  } catch (cl::sycl::exception const &e) {
    std::cout << "SYCL exception caught: " << e.what() << '\n';
    free(points4, q);
    free(centroids4, q);
    free(accum4, q);
    free(points, q);
    free(centroids, q);
    return e.get_cl_code();
  }

  //---

  auto correct = verify_result(centroids4, centroids);

  std::cout << std::endl;

  float kernel1_time = kernel1_time_in_ms;
  float kernel2_time = kernel2_time_in_ms;
  float kernel_time = kernel1_time + kernel2_time;

  printf("\n--- ESIMD Kernel execution stats begin ---\n");

  printf("NUMBER_OF_POINTS: %d\n", NUM_POINTS);
  printf("NUMBER_OF_CENTROIDS: %d\n", NUM_CENTROIDS_ACTUAL);
  printf("NUMBER_OF_ITERATIONS: %d\n", NUM_ITERATIONS);
  printf("POINTS_PER_THREAD: %d\n", POINTS_PER_THREAD);

  printf("Average kernel1 time: %f ms\n", kernel1_time / NUM_ITERATIONS);
  printf("Total kernel1 time: %f ms\n\n", kernel1_time);

  printf("Average kernel2 time: %f ms\n", kernel2_time / NUM_ITERATIONS);
  printf("Total kernel2 time: %f ms\n\n", kernel2_time);

  printf("Average kernel time: %f ms\n", kernel_time / NUM_ITERATIONS);
  printf("Total kernel time: %f ms\n\n", kernel_time);

  printf("--- ESIMD Kernel execution stats end ---\n\n");

  std::cout << std::endl;

  std::cout << ((correct) ? "PASSED" : "FAILED") << std::endl;

  free(points4, q);
  free(centroids4, q);
  free(accum4, q);
  free(points, q);
  free(centroids, q);

  return !correct;
}
