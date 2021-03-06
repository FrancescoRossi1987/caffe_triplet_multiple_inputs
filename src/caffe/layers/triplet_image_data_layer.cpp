#ifdef USE_OPENCV
#include <opencv2/core/core.hpp>

#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>

#include "caffe/layers/triplet_image_data_layer.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
TripletImageDataLayer<Dtype>::~TripletImageDataLayer<Dtype>() {
  this->StopInternalThread();
}

template <typename Dtype>
void TripletImageDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int new_height = this->layer_param_.image_data_param().new_height();
  const int new_width  = this->layer_param_.image_data_param().new_width();
  const bool is_color  = this->layer_param_.image_data_param().is_color();
  string root_folder = this->layer_param_.image_data_param().root_folder();

  CHECK((new_height == 0 && new_width == 0) ||
      (new_height > 0 && new_width > 0)) << "Current implementation requires "
      "new_height and new_width to be set at the same time.";
  // Read the file with filenames
  const string& source = this->layer_param_.image_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());
  vector<string> filenames(3);
  while (infile >> filenames[0] >> filenames[1] >> filenames[2]) {
    lines_.push_back(filenames);
  }

  if (this->layer_param_.image_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleTriplets();
  }
  LOG(INFO) << "A total of " << lines_.size() << " images.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.image_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.image_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }
  // Read an image, and use it to initialize the top blob.
  int tag = 0;
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][0], new_height, new_width, is_color);
  if( !cv_img.data ){
     cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][1], new_height, new_width, is_color);
     tag = 1;
     if( !cv_img.data ){
       cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][2], new_height, new_width, is_color);
       tag = 2;
     }
  }
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_][tag];//it is aweful if all the three image are bad images;
  // Use data_transformer to infer the expected blob shape from a cv_image.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);
  // Reshape prefetch_data and top[0] according to the batch_size.
  const int batch_size = this->layer_param_.image_data_param().batch_size();
  CHECK_GT(batch_size, 0) << "Positive batch size required";
  top_shape[0] = batch_size * 3;
  for (int i = 0; i < this->PREFETCH_COUNT; ++i) {
    this->prefetch_[i].data_.Reshape(top_shape);
  }
  top[0]->Reshape(top_shape);

  LOG(INFO) << "output data size: " << top[0]->num() << ","
      << top[0]->channels() << "," << top[0]->height() << ","
      << top[0]->width();
}

template <typename Dtype>
void TripletImageDataLayer<Dtype>::ShuffleTriplets() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}

// This function is called on prefetch thread
template <typename Dtype>
void TripletImageDataLayer<Dtype>::load_batch(Batch<Dtype>* batch) {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(batch->data_.count());
  CHECK(this->transformed_data_.count());
  ImageDataParameter image_data_param = this->layer_param_.image_data_param();
  const int batch_size = image_data_param.batch_size();
  const int new_height = image_data_param.new_height();
  const int new_width = image_data_param.new_width();
  const bool is_color = image_data_param.is_color();
  string root_folder = image_data_param.root_folder();

  // Reshape according to the first image of each batch
  // on single input batches allows for inputs of varying dimension.
  //
  int tag = 0;
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][0], new_height, new_width, is_color);
  if( !cv_img.data ){
    cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][1], new_height, new_width, is_color);
    tag = 1;
    if( !cv_img.data ){
      cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][2], new_height, new_width, is_color);
      tag = 2;
    }
  }
  CHECK(cv_img.data) << "Could not load " << lines_[lines_id_][tag];
  // Use data_transformer to infer the expected blob shape from a cv_img.
  vector<int> top_shape = this->data_transformer_->InferBlobShape(cv_img);
  this->transformed_data_.Reshape(top_shape);
  // Reshape batch according to the batch_size.
  top_shape[0] = batch_size * 3;
  batch->data_.Reshape(top_shape);

  Dtype* prefetch_data = batch->data_.mutable_cpu_data();

  // datum scales
  const int lines_size = lines_.size();
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    for (int tri_id=0; tri_id<3; ++tri_id) {
      // get a blob
      timer.Start();
      CHECK_GT(lines_size, lines_id_);
      cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_][tri_id],
          new_height, new_width, is_color);
      if( !cv_img.data ){
        item_id--;
        break;
      }
      CHECK(cv_img.data) << "Could not load " << lines_[lines_id_][tri_id];
      read_time += timer.MicroSeconds();
      timer.Start();
      // Apply transformations (mirror, crop...) to the image
      int offset = batch->data_.offset(item_id + tri_id * batch_size);
      this->transformed_data_.set_cpu_data(prefetch_data + offset);
      this->data_transformer_->Transform(cv_img, &(this->transformed_data_));
      trans_time += timer.MicroSeconds();
    }

    // go to the next iter
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.image_data_param().shuffle()) {
        ShuffleTriplets();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(TripletImageDataLayer);
REGISTER_LAYER_CLASS(TripletImageData);

}  // namespace caffe
#endif  // USE_OPENCV
