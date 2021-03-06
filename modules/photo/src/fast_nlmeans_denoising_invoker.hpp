/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                        Intel License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective icvers.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of Intel Corporation may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#ifndef __OPENCV_FAST_NLMEANS_DENOISING_INVOKER_HPP__
#define __OPENCV_FAST_NLMEANS_DENOISING_INVOKER_HPP__

#include "precomp.hpp"
#include <opencv2/core/core.hpp>
#include <opencv2/core/internal.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <limits>

#include "fast_nlmeans_denoising_invoker_commons.hpp"
#include "arrays.hpp"

using namespace std;
using namespace cv;

template <typename T>
struct FastNlMeansDenoisingInvoker {
    public:     
        FastNlMeansDenoisingInvoker(const Mat& src, Mat& dst, 
            int template_window_size, int search_window_size, const double h);

        void operator() (const BlockedRange& range) const;

		void operator= (const FastNlMeansDenoisingInvoker&) {
			CV_Error(CV_StsNotImplemented, "Assigment operator is not implemented");
		}

    private:
        const Mat& src_;
        Mat& dst_;

        Mat extended_src_;
        int border_size_;

        int template_window_size_;
        int search_window_size_;

        int template_window_half_size_;
        int search_window_half_size_;

        int fixed_point_mult_;
        int almost_template_window_size_sq_bin_shift;
        vector<int> almost_dist2weight;

        void calcDistSumsForFirstElementInRow(
            int i, 
            Array2d<int>& dist_sums, 
            Array3d<int>& col_dist_sums, 
            Array3d<int>& up_col_dist_sums) const; 

        void calcDistSumsForElementInFirstRow(
            int i,
            int j, 
            int first_col_num,
            Array2d<int>& dist_sums, 
            Array3d<int>& col_dist_sums, 
            Array3d<int>& up_col_dist_sums) const;         
};

template <class T>
FastNlMeansDenoisingInvoker<T>::FastNlMeansDenoisingInvoker(
    const cv::Mat& src, 
    cv::Mat& dst, 
    int template_window_size, 
    int search_window_size, 
    const double h) : src_(src), dst_(dst)
{
    CV_Assert(src.channels() <= 3);

    template_window_half_size_ = template_window_size / 2;
    search_window_half_size_ = search_window_size / 2;
    template_window_size_ = template_window_half_size_ * 2 + 1;
    search_window_size_ = search_window_half_size_ * 2 + 1;

    border_size_ = search_window_half_size_ + template_window_half_size_;
    copyMakeBorder(src_, extended_src_, 
        border_size_, border_size_, border_size_, border_size_, cv::BORDER_DEFAULT);

    const int max_estimate_sum_value = search_window_size_ * search_window_size_ * 255;
    fixed_point_mult_ = numeric_limits<int>::max() / max_estimate_sum_value;

    // precalc weight for every possible l2 dist between blocks
    // additional optimization of precalced weights to replace division(averaging) by binary shift
    int template_window_size_sq = template_window_size_ * template_window_size_;
    almost_template_window_size_sq_bin_shift = 0;
    while (1 << almost_template_window_size_sq_bin_shift < template_window_size_sq) {
        almost_template_window_size_sq_bin_shift++;
    }
    
    int almost_template_window_size_sq = 1 << almost_template_window_size_sq_bin_shift;
    double almost_dist2actual_dist_multiplier = 
        ((double) almost_template_window_size_sq) / template_window_size_sq;

    int max_dist = 256 * 256 * src_.channels();
    int almost_max_dist = (int) (max_dist / almost_dist2actual_dist_multiplier + 1);
    almost_dist2weight.resize(almost_max_dist);

    const double WEIGHT_THRESHOLD = 0.001;
    for (int almost_dist = 0; almost_dist < almost_max_dist; almost_dist++) {
        double dist = almost_dist * almost_dist2actual_dist_multiplier;
        int weight = cvRound(fixed_point_mult_ * std::exp(- dist / (h * h * src_.channels())));

        if (weight < WEIGHT_THRESHOLD * fixed_point_mult_) {
            weight = 0;
        }

        almost_dist2weight[almost_dist] = weight;
    }
    // additional optimization init end

    if (dst_.empty()) {
        dst_ = Mat::zeros(src_.size(), src_.type());
    }
}

template <class T>
void FastNlMeansDenoisingInvoker<T>::operator() (const BlockedRange& range) const {
    int row_from = range.begin();
    int row_to = range.end() - 1;

    Array2d<int> dist_sums(search_window_size_, search_window_size_);
    
    // for lazy calc optimization
    Array3d<int> col_dist_sums(template_window_size_, search_window_size_, search_window_size_);
    
    int first_col_num = -1;
    Array3d<int> up_col_dist_sums(src_.cols, search_window_size_, search_window_size_);

    for (int i = row_from; i <= row_to; i++) {
        for (int j = 0; j < src_.cols; j++) {
            int search_window_y = i - search_window_half_size_;
            int search_window_x = j - search_window_half_size_;

            // calc dist_sums
            if (j == 0) {
                calcDistSumsForFirstElementInRow(i, dist_sums, col_dist_sums, up_col_dist_sums);
                first_col_num = 0;

            } else { // calc cur dist_sums using previous dist_sums
                if (i == row_from) {
                    calcDistSumsForElementInFirstRow(i, j, first_col_num, 
                        dist_sums, col_dist_sums, up_col_dist_sums);    

                } else {
                    int ay = border_size_ + i; 
                    int ax = border_size_ + j + template_window_half_size_;

                    int start_by = 
                        border_size_ + i - search_window_half_size_;

                    int start_bx = 
                        border_size_ + j - search_window_half_size_ + template_window_half_size_;

                    T a_up = extended_src_.at<T>(ay - template_window_half_size_ - 1, ax);
                    T a_down = extended_src_.at<T>(ay + template_window_half_size_, ax);

                    // copy class member to local variable for optimization
                    int search_window_size = search_window_size_;

                    for (int y = 0; y < search_window_size; y++) {
                        int* dist_sums_row = dist_sums.row_ptr(y);
                        
                        int* col_dist_sums_row = col_dist_sums.row_ptr(first_col_num,y);
                        
                        int* up_col_dist_sums_row = up_col_dist_sums.row_ptr(j, y);

                        const T* b_up_ptr = 
                            extended_src_.ptr<T>(start_by - template_window_half_size_ - 1 + y);

                        const T* b_down_ptr = 
                            extended_src_.ptr<T>(start_by + template_window_half_size_ + y);
                        
                        for (int x = 0; x < search_window_size; x++) {
                            dist_sums_row[x] -= col_dist_sums_row[x];
                        
                            col_dist_sums_row[x] = 
                                up_col_dist_sums_row[x] + 
                                calcUpDownDist(
                                    a_up, a_down, 
                                    b_up_ptr[start_bx + x], b_down_ptr[start_bx + x]
                                );

                            dist_sums_row[x] += col_dist_sums_row[x];
                            
                            up_col_dist_sums_row[x] = col_dist_sums_row[x];
                            
                        }
                    }
                }
                
                first_col_num = (first_col_num + 1) % template_window_size_;
            }

            // calc weights
            int weights_sum = 0;
            
            int estimation[3];            
            for (int channel_num = 0; channel_num < src_.channels(); channel_num++) {
                estimation[channel_num] = 0;
            }
            
            for (int y = 0; y < search_window_size_; y++) {
                const T* cur_row_ptr = extended_src_.ptr<T>(border_size_ + search_window_y + y);
                int* dist_sums_row = dist_sums.row_ptr(y);
                for (int x = 0; x < search_window_size_; x++) {
                    int almostAvgDist = 
                        dist_sums_row[x] >> almost_template_window_size_sq_bin_shift;

                    int weight = almost_dist2weight[almostAvgDist];
                    weights_sum += weight;
                    
                    T p = cur_row_ptr[border_size_ + search_window_x + x];
                    incWithWeight(estimation, weight, p);
                }
            }
            
            if (weights_sum > 0) {
                for (int channel_num = 0; channel_num < src_.channels(); channel_num++) {
                    estimation[channel_num] = 
                        cvRound(((double)estimation[channel_num]) / weights_sum);
                }

                dst_.at<T>(i,j) = saturateCastFromArray<T>(estimation);

            } else { // weights_sum == 0
                dst_.at<T>(i,j) = src_.at<T>(i,j);
            }
        }
    }
}

template <class T>
inline void FastNlMeansDenoisingInvoker<T>::calcDistSumsForFirstElementInRow(
    int i, 
    Array2d<int>& dist_sums, 
    Array3d<int>& col_dist_sums, 
    Array3d<int>& up_col_dist_sums) const
{
    int j = 0;

    for (int y = 0; y < search_window_size_; y++) {
        for (int x = 0; x < search_window_size_; x++) {
            dist_sums[y][x] = 0;
            for (int tx = 0; tx < template_window_size_; tx++) {
                col_dist_sums[tx][y][x] = 0;
            }

            int start_y = i + y - search_window_half_size_;
            int start_x = j + x - search_window_half_size_;

            for (int ty = -template_window_half_size_; ty <= template_window_half_size_; ty++) {
                for (int tx = -template_window_half_size_; tx <= template_window_half_size_; tx++) {
                    int dist = calcDist<T>(extended_src_, 
                        border_size_ + i + ty, border_size_ + j + tx,
                        border_size_ + start_y + ty, border_size_ + start_x + tx);

                    dist_sums[y][x] += dist;
                    col_dist_sums[tx + template_window_half_size_][y][x] += dist;
                }
            }

            up_col_dist_sums[j][y][x] = col_dist_sums[template_window_size_ - 1][y][x];
        }
    }
}

template <class T>
inline void FastNlMeansDenoisingInvoker<T>::calcDistSumsForElementInFirstRow(
    int i,
    int j,
    int first_col_num,
    Array2d<int>& dist_sums, 
    Array3d<int>& col_dist_sums, 
    Array3d<int>& up_col_dist_sums) const
{
    int ay = border_size_ + i; 
    int ax = border_size_ + j + template_window_half_size_;

    int start_by = border_size_ + i - search_window_half_size_;
    int start_bx = border_size_ + j - search_window_half_size_ + template_window_half_size_;
    
    int new_last_col_num = first_col_num;

    for (int y = 0; y < search_window_size_; y++) {
        for (int x = 0; x < search_window_size_; x++) {
            dist_sums[y][x] -= col_dist_sums[first_col_num][y][x];
        
            col_dist_sums[new_last_col_num][y][x] = 0;                      
            int by = start_by + y; 
            int bx = start_bx + x;
            for (int ty = -template_window_half_size_; ty <= template_window_half_size_; ty++) {
                col_dist_sums[new_last_col_num][y][x] += 
                    calcDist<T>(extended_src_, ay + ty, ax, by + ty, bx);
            }   

            dist_sums[y][x] += col_dist_sums[new_last_col_num][y][x];

            up_col_dist_sums[j][y][x] = col_dist_sums[new_last_col_num][y][x];
        }
    }
}

#endif
