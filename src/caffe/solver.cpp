#include <cstdio>

#include <algorithm>
#include <string>
#include <vector>

#include "caffe/net.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/solver.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/upgrade_proto.hpp"

namespace caffe {

template <typename Dtype>
Solver<Dtype>::Solver(const SolverParameter &param)
    : net_(), max_acc_(0), is_max_(false) {
  Init(param);
}

template <typename Dtype>
Solver<Dtype>::Solver(const string &param_file)
    : net_(), max_acc_(0), is_max_(false) {
  SolverParameter param;
  ReadProtoFromTextFileOrDie(param_file, &param);
  Init(param);
}

template <typename Dtype>
void Solver<Dtype>::Init(const SolverParameter &param) {
  LOG(INFO) << "Initializing solver from parameters: " << std::endl
            << param.DebugString();
  param_ = param;
  if (param_.random_seed() >= 0) {
    Caffe::set_random_seed(param_.random_seed());
  }
  // Scaffolding code
  InitTrainNet();
  InitTestNets();
  LOG(INFO) << "base lr = " << param_.base_lr()
            << " global weight decay = " << param_.weight_decay()
            << " momentum = " << param_.momentum();
  LOG(INFO) << "Solver scaffolding done.";
}

template <typename Dtype> void Solver<Dtype>::InitTrainNet() {
  const int num_train_nets = param_.has_net() + param_.has_net_param() +
                             param_.has_train_net() +
                             param_.has_train_net_param();
  const string &field_names = "net, net_param, train_net, train_net_param";
  CHECK_GE(num_train_nets, 1) << "SolverParameter must specify a train net "
                              << "using one of these fields: " << field_names;
  CHECK_LE(num_train_nets, 1)
      << "SolverParameter must not contain more than "
      << "one of these fields specifying a train_net: " << field_names;
  NetParameter net_param;
  if (param_.has_train_net_param()) {
    LOG(INFO) << "Creating training net specified in train_net_param.";
    net_param.CopyFrom(param_.train_net_param());
  } else if (param_.has_train_net()) {
    LOG(INFO) << "Creating training net from train_net file: "
              << param_.train_net();
    ReadNetParamsFromTextFileOrDie(param_.train_net(), &net_param);
  }
  if (param_.has_net_param()) {
    LOG(INFO) << "Creating training net specified in net_param.";
    net_param.CopyFrom(param_.net_param());
  }
  if (param_.has_net()) {
    LOG(INFO) << "Creating training net from net file: " << param_.net();
    ReadNetParamsFromTextFileOrDie(param_.net(), &net_param);
  }
  // Set the correct NetState.  We start with the solver defaults (lowest
  // precedence); then, merge in any NetState specified by the net_param itself;
  // finally, merge in any NetState specified by the train_state (highest
  // precedence).
  NetState net_state;
  net_state.set_phase(TRAIN);
  net_state.MergeFrom(net_param.state());
  net_state.MergeFrom(param_.train_state());
  net_param.mutable_state()->CopyFrom(net_state);
  net_.reset(new Net<Dtype>(net_param));
}

template <typename Dtype> void Solver<Dtype>::InitTestNets() {
  const bool has_net_param = param_.has_net_param();
  const bool has_net_file = param_.has_net();
  const int num_generic_nets = has_net_param + has_net_file;
  CHECK_LE(num_generic_nets, 1)
      << "Both net_param and net_file may not be specified.";
  const int num_test_net_params = param_.test_net_param_size();
  const int num_test_net_files = param_.test_net_size();
  const int num_test_nets = num_test_net_params + num_test_net_files;
  if (num_generic_nets) {
    CHECK_GE(param_.test_iter_size(), num_test_nets)
        << "test_iter must be specified for each test network.";
  } else {
    CHECK_EQ(param_.test_iter_size(), num_test_nets)
        << "test_iter must be specified for each test network.";
  }
  // If we have a generic net (specified by net or net_param, rather than
  // test_net or test_net_param), we may have an unlimited number of actual
  // test networks -- the actual number is given by the number of remaining
  // test_iters after any test nets specified by test_net_param and/or test_net
  // are evaluated.
  const int num_generic_net_instances = param_.test_iter_size() - num_test_nets;
  const int num_test_net_instances = num_test_nets + num_generic_net_instances;
  if (param_.test_state_size()) {
    CHECK_EQ(param_.test_state_size(), num_test_net_instances)
        << "test_state must be unspecified or specified once per test net.";
  }
  if (num_test_net_instances) {
    CHECK_GT(param_.test_interval(), 0);
  }
  int test_net_id = 0;
  vector<string> sources(num_test_net_instances);
  vector<NetParameter> net_params(num_test_net_instances);
  for (int i = 0; i < num_test_net_params; ++i, ++test_net_id) {
    sources[test_net_id] = "test_net_param";
    net_params[test_net_id].CopyFrom(param_.test_net_param(i));
  }
  for (int i = 0; i < num_test_net_files; ++i, ++test_net_id) {
    sources[test_net_id] = "test_net file: " + param_.test_net(i);
    ReadNetParamsFromTextFileOrDie(param_.test_net(i),
                                   &net_params[test_net_id]);
  }
  const int remaining_test_nets = param_.test_iter_size() - test_net_id;
  if (has_net_param) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net_param";
      net_params[test_net_id].CopyFrom(param_.net_param());
    }
  }
  if (has_net_file) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net file: " + param_.net();
      ReadNetParamsFromTextFileOrDie(param_.net(), &net_params[test_net_id]);
    }
  }
  test_nets_.resize(num_test_net_instances);
  for (int i = 0; i < num_test_net_instances; ++i) {
    // Set the correct NetState.  We start with the solver defaults (lowest
    // precedence); then, merge in any NetState specified by the net_param
    // itself; finally, merge in any NetState specified by the test_state
    // (highest precedence).
    NetState net_state;
    net_state.set_phase(TEST);
    net_state.MergeFrom(net_params[i].state());
    if (param_.test_state_size()) {
      net_state.MergeFrom(param_.test_state(i));
    }
    net_params[i].mutable_state()->CopyFrom(net_state);
    LOG(INFO)
        << "Creating test net (#" << i << ") specified by " << sources[i];
    test_nets_[i].reset(new Net<Dtype>(net_params[i]));
  }
}

template <typename Dtype> void Solver<Dtype>::Solve(const char *resume_file) {
  Caffe::set_phase(Caffe::TRAIN);
  LOG(INFO) << "Solving " << net_->name();
  PreSolve();
  iter_ = 0;
  // if epoch_size is set, we will start using epoch to count 1 iteration
  epoch_ = (param_.epoch_size() > 0) ? 0 : -1;

  if (resume_file) {
    LOG(INFO) << "Restoring previous solver status from " << resume_file;
    Restore(resume_file);
  }
  // Remember the initial iter_ value; will be non-zero if we loaded from a
  // resume_file above.
  const int start_iter = iter_;
  const int start_epoch = epoch_;

  // For a network that is trained by the solver, no bottom or top vecs
  // should be given, and we will just provide dummy vecs.
  vector<Blob<Dtype> *> bottom_vec;

  int *counter = (param_.epoch_size() > 0) ? &epoch_ : &iter_;
  while (*counter < param_.max_iter()) {
    ++iter_;
    // if epoch_size is set, count epoch
    if (param_.epoch_size() > 0 && iter_ % param_.epoch_size() == 0)
      ++epoch_;

    // Save a snapshot if needed.
    if (param_.snapshot() && ((epoch_ > 0 && epoch_ > start_epoch &&
			       iter_ % param_.epoch_size() == 0 &&
                               epoch_ % param_.snapshot() == 0) ||
                              (epoch_ < 0 && iter_ > start_iter &&
                               iter_ % param_.snapshot() == 0))) {
      Snapshot();
    }

    // if epoch is available, use epoch-iteration to report, o.w. use iteration
    // as default
    bool do_test = false;
    if (param_.epoch_size() > 0) {
      do_test = param_.test_interval() && epoch_ % param_.test_interval() == 0
	&& iter_ % param_.epoch_size() == 0
	&& (epoch_ > start_epoch || param_.test_initialization());
    } else { // Using iteration
      do_test = param_.test_interval() && iter_ % param_.test_interval() == 0
        && (iter_ > 0 || param_.test_initialization());
    }
    if (do_test) {
      TestAll();
    }

    const bool display =
        (epoch_ < 0 && param_.display() && iter_ % param_.display() == 0) ||
        (epoch_ > 0 && epoch_ % param_.display() == 0 &&
         iter_ % param_.epoch_size() == 0);
    net_->set_debug_info(display && param_.debug_info());
    Dtype loss = net_->ForwardBackward(bottom_vec);
    if (display) {
      net_->Report();
      if (epoch_ < 0) {
        LOG(INFO) << "Iteration " << iter_ << ", loss = " << loss;
      } else {
        LOG(INFO) << "Epoch " << epoch_ << " (iteration " << iter_
                  << "), loss = " << loss;
      }
      const vector<Blob<Dtype>*>& result = net_->output_blobs();
      int score_index = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        const string& output_name =
            net_->blob_names()[net_->output_blob_indices()[j]];
        const Dtype loss_weight =
            net_->blob_loss_weights()[net_->output_blob_indices()[j]];
        for (int k = 0; k < result[j]->count(); ++k) {
          ostringstream loss_msg_stream;
          if (loss_weight) {
            loss_msg_stream << " (* " << loss_weight
                            << " = " << loss_weight * result_vec[k] << " loss)";
          }
          LOG(INFO) << "    Train net output #"
              << score_index++ << ": " << output_name << " = "
              << result_vec[k] << loss_msg_stream.str();
        }
      }
    }

    ComputeUpdateValue();
    net_->Update();
  }
  --iter_;
  // Always save a snapshot after optimization, unless overridden by setting
  // snapshot_after_train := false.
  if (param_.snapshot_after_train()) {
    Snapshot();
  }
  // After the optimization is done, run an additional train and test pass to
  // display the train and test loss/outputs if appropriate (based on the
  // display and test_interval settings, respectively).  Unlike in the rest of
  // training, for the train net we only run a forward pass as we've already
  // updated the parameters "max_iter" times -- this final pass is only done to
  // display the loss, which is computed in the forward pass.
  if (param_.display() && iter_ % param_.display() == 0) {
    Dtype loss;
    net_->Forward(bottom_vec, &loss);
    LOG(INFO) << "Iteration " << iter_ << ", loss = " << loss;
  }
  if (param_.test_interval() && iter_ % param_.test_interval() == 0) {
    TestAll();
  }
  LOG(INFO) << "Optimization Done.";
}

template <typename Dtype> void Solver<Dtype>::TestAll() {
  for (int test_net_id = 0; test_net_id < test_nets_.size(); ++test_net_id) {
    Test(test_net_id);
  }
}

template <typename Dtype> void Solver<Dtype>::Test(const int test_net_id) {
  if (epoch_ < 0) {
    LOG(INFO) << "Iteration " << iter_ << ", Testing net (#" << test_net_id
              << ")";
  } else {
    LOG(INFO) << "Epoch " << epoch_ << ", Testing net (#" << test_net_id << ")";
  }
  // We need to set phase to test before running.
  Caffe::set_phase(Caffe::TEST);
  CHECK_NOTNULL(test_nets_[test_net_id].get())
      ->ShareTrainedLayersWith(net_.get());
  vector<Dtype> test_score;
  vector<int> test_score_output_id;
  vector<Blob<Dtype>*> bottom_vec;
  const shared_ptr<Net<Dtype> >& test_net = test_nets_[test_net_id];
  Dtype loss = 0;
  for (int i = 0; i < param_.test_iter(test_net_id); ++i) {
    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result =
        test_net->Forward(bottom_vec, &iter_loss);
    if (param_.test_compute_loss()) {
      loss += iter_loss;
    }
    if (i == 0) {
      for (int j = 0; j < result.size(); ++j) {
        const Dtype *result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score.push_back(result_vec[k]);
          test_score_output_id.push_back(j);
        }
      }
    } else {
      int idx = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype *result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score[idx++] += result_vec[k];
        }
      }
    }
  }

  if (param_.test_compute_loss()) {
    loss /= param_.test_iter(test_net_id);
    LOG(INFO) << "Test loss: " << loss;
  }
  for (int i = 0; i < test_score.size(); ++i) {
    const int output_blob_index =
        test_net->output_blob_indices()[test_score_output_id[i]];
    const string& output_name = test_net->blob_names()[output_blob_index];
    const Dtype loss_weight = test_net->blob_loss_weights()[output_blob_index];
    ostringstream loss_msg_stream;
    const Dtype mean_score = test_score[i] / param_.test_iter(test_net_id);
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    if (i == 0) {
      LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
		<< mean_score << " (" << 100 * (1 - mean_score) << "% error)"
		<<loss_msg_stream.str();
    } else {
      LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
		<< mean_score << loss_msg_stream.str();
    }

  }
  Caffe::set_phase(Caffe::TRAIN);
  CHECK(test_score.size() > 0) << "test net doesn't produce any output!";

  // AJ: Save max acc if required.
  Dtype acc = 1 - (test_score[0] / param_.test_iter(test_net_id));
  if (param_.save_max() && acc >= max_acc_) {
    max_acc_ = acc;
    // if we got the best score, snapshot it.
    if (param_.epoch_size() > 0) {
      LOG(INFO) << "Best score: " << max_acc_ << " at Epoch " << epoch_;
    } else {
      LOG(INFO) << "Best score: " << max_acc_ << " at Iteration " << iter_;
    }

    is_max_ = true;
    Snapshot();
    is_max_ = false;
  }
}

template <typename Dtype> void Solver<Dtype>::Snapshot() {
  NetParameter net_param;
  // For intermediate results, we will also dump the gradient values.
  net_->ToProto(&net_param, param_.snapshot_diff());
  string filename(param_.snapshot_prefix());
  string model_filename, snapshot_filename;
  const int kBufferSize = 20;
  char iter_str_buffer[kBufferSize];
  if (is_max_) {
    float error = (1 - max_acc_) * 100;
    sprintf(iter_str_buffer, "_best_%.2f", error);
  } else if (epoch_ > 0) {
    // if epoch is set, save with epoch size
    sprintf(iter_str_buffer, "_epoch_%d", epoch_);
  } else {
    sprintf(iter_str_buffer, "_iter_%d", iter_);
  }

  filename += iter_str_buffer;
  model_filename = filename + ".caffemodel";
  if (!is_max_) LOG(INFO) << "Snapshotting to " << model_filename;
  WriteProtoToBinaryFile(net_param, model_filename.c_str());
  SolverState state;
  SnapshotSolverState(&state);
  if (param_.save_max()) state.set_max_val(max_acc_);
  state.set_iter(iter_);
  state.set_learned_net(model_filename);
  snapshot_filename = filename + ".solverstate";
  LOG(INFO) << "Snapshotting solver state to " << snapshot_filename;
  WriteProtoToBinaryFile(state, snapshot_filename.c_str());
}

template <typename Dtype> void Solver<Dtype>::Restore(const char *state_file) {
  SolverState state;
  NetParameter net_param;
  ReadProtoFromBinaryFile(state_file, &state);
  if (state.has_learned_net()) {
    ReadProtoFromBinaryFile(state.learned_net().c_str(), &net_param);
    net_->CopyTrainedLayersFrom(net_param);
  }
  iter_ = state.iter();
  epoch_ = (param_.epoch_size() > 0) ? iter_ / param_.epoch_size() : -1;
  max_acc_ = state.has_max_val() ? state.max_val() : 0;
  LOG(INFO) << "Starting from epoch " << epoch_ << " (iter " << iter_ << " )";
  RestoreSolverState(state);
}

// Return the current learning rate. The currently implemented learning rate
// policies are as follows:
//    - fixed: always return base_lr.
//    - step: return base_lr * gamma ^ (floor(iter / step))
//    - exp: return base_lr * gamma ^ iter
//    - inv: return base_lr * (1 + gamma * iter) ^ (- power)
// AJ: - exp_after: return base_lr * gamma ^ (iter - power) if iter > power else
// base_lr
//  (i.e. start annealing after power many iterations)
// where base_lr, gamma, step and power are defined in the solver parameter
// protocol buffer, and iter is the current iteration/epoch.
template <typename Dtype> Dtype SGDSolver<Dtype>::GetLearningRate() {
  Dtype rate;
  const string &lr_policy = this->param_.lr_policy();
  // Use epoch if its used instead of iter
  int itr = (this->param_.epoch_size() > 0) ? this->epoch_ : this->iter_;
  if (lr_policy == "fixed") {
    rate = this->param_.base_lr();
  } else if (lr_policy == "step") {
    int current_step = itr / this->param_.stepsize();
    rate = this->param_.base_lr() * pow(this->param_.gamma(), current_step);
  } else if (lr_policy == "exp") {
    rate = this->param_.base_lr() * pow(this->param_.gamma(), itr);
  } else if (lr_policy == "exp_after") {
    rate = itr > this->param_.power()
               ? this->param_.base_lr() *
                     pow(this->param_.gamma(), itr - this->param_.power())
               : this->param_.base_lr();
  } else if (lr_policy == "inv") {
    rate = this->param_.base_lr() *
           pow(Dtype(1) + this->param_.gamma() * itr, -this->param_.power());
  } else {
    LOG(FATAL) << "Unknown learning rate policy: " << lr_policy;
  }

  // print current lr if it's not fixed
  if (lr_policy != "fixed") {
    if (this->epoch_ > 0 && this->iter_ % this->param_.epoch_size() == 0 &&
        this->epoch_ % this->param_.display() == 0) {
      LOG(INFO) << "Epoch " << this->epoch_ << ", lr = " << rate;
    } else if (this->epoch_ < 0 && this->param_.display() &&
               this->iter_ % this->param_.display() == 0) {
      LOG(INFO) << "Iteration " << this->iter_ << ", lr = " << rate;
    }
  }

  return rate;
}

template <typename Dtype> void SGDSolver<Dtype>::PreSolve() {
  // Initialize the history
  vector<shared_ptr<Blob<Dtype> > > &net_params = this->net_->params();
  history_.clear();
  update_.clear();
  temp_.clear();
  for (int i = 0; i < net_params.size(); ++i) {
    const Blob<Dtype>* net_param = net_params[i].get();
    history_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(
        net_param->num(), net_param->channels(), net_param->height(),
        net_param->width())));
    update_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(
        net_param->num(), net_param->channels(), net_param->height(),
        net_param->width())));
    temp_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(
        net_param->num(), net_param->channels(), net_param->height(),
        net_param->width())));
  }
}

template <typename Dtype> void SGDSolver<Dtype>::ComputeUpdateValue() {
  vector<shared_ptr<Blob<Dtype> > > &net_params = this->net_->params();
  vector<float> &net_params_lr = this->net_->params_lr();
  vector<float> &net_params_weight_decay = this->net_->params_weight_decay();
  // get the learning rate
  Dtype rate = GetLearningRate();

  Dtype momentum = this->param_.momentum();
  Dtype weight_decay = this->param_.weight_decay();
  string regularization_type = this->param_.regularization_type();
  switch (Caffe::mode()) {
  case Caffe::CPU:
    for (int param_id = 0; param_id < net_params.size(); ++param_id) {
      // Compute the value to history, and then copy them to the blob's diff.
      Dtype local_rate = rate * net_params_lr[param_id];
      Dtype local_decay = weight_decay * net_params_weight_decay[param_id];
      if (local_decay) {
        if (regularization_type == "L2") {
          // add weight decay
	  // AJ: void caffe_axpby<float>(const int N, const float alpha, const
	  // float* X,const float beta, float* Y)
          caffe_axpy(net_params[param_id]->count(),
              local_decay,
              net_params[param_id]->cpu_data(),
              net_params[param_id]->mutable_cpu_diff());
        } else if (regularization_type == "L1") {
          caffe_cpu_sign(net_params[param_id]->count(),
              net_params[param_id]->cpu_data(),
              temp_[param_id]->mutable_cpu_data());
          caffe_axpy(net_params[param_id]->count(),
              local_decay,
              temp_[param_id]->cpu_data(),
              net_params[param_id]->mutable_cpu_diff());
        } else {
          LOG(FATAL) << "Unknown regularization type: " << regularization_type;
        }
      }

      caffe_cpu_axpby(net_params[param_id]->count(), local_rate,
                net_params[param_id]->cpu_diff(), momentum,
                history_[param_id]->mutable_cpu_data());
      // copy
      // caffe_copy = cblas_dcopy(N, X, 1, Y, 1)
      caffe_copy(net_params[param_id]->count(), history_[param_id]->cpu_data(),
                 net_params[param_id]->mutable_cpu_diff());
    }
    break;
  case Caffe::GPU:
#ifndef CPU_ONLY
    for (int param_id = 0; param_id < net_params.size(); ++param_id) {
      // Compute the value to history, and then copy them to the blob's diff.
      Dtype local_rate = rate * net_params_lr[param_id];
      Dtype local_decay = weight_decay * net_params_weight_decay[param_id];
      if (local_decay) {
        if (regularization_type == "L2") {
          // add weight decay
          caffe_gpu_axpy(net_params[param_id]->count(),
              local_decay,
              net_params[param_id]->gpu_data(),
              net_params[param_id]->mutable_gpu_diff());
        } else if (regularization_type == "L1") {
          caffe_gpu_sign(net_params[param_id]->count(),
              net_params[param_id]->gpu_data(),
              temp_[param_id]->mutable_gpu_data());
          caffe_gpu_axpy(net_params[param_id]->count(),
              local_decay,
              temp_[param_id]->gpu_data(),
              net_params[param_id]->mutable_gpu_diff());
        } else {
          LOG(FATAL) << "Unknown regularization type: " << regularization_type;
        }
      }

      caffe_gpu_axpby(net_params[param_id]->count(), local_rate,
                net_params[param_id]->gpu_diff(), momentum,
                history_[param_id]->mutable_gpu_data());
      // copy
      caffe_copy(net_params[param_id]->count(), history_[param_id]->gpu_data(),
                 net_params[param_id]->mutable_gpu_diff());
    }
#else
    NO_GPU;
#endif
    break;
  default:
    LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
  }
}

template <typename Dtype>
void SGDSolver<Dtype>::SnapshotSolverState(SolverState *state) {
  state->clear_history();
  for (int i = 0; i < history_.size(); ++i) {
    // Add history
    BlobProto *history_blob = state->add_history();
    history_[i]->ToProto(history_blob);
  }
}

template <typename Dtype>
void SGDSolver<Dtype>::RestoreSolverState(const SolverState &state) {
  CHECK_EQ(state.history_size(), history_.size())
      << "Incorrect length of history blobs.";
  LOG(INFO) << "SGDSolver: restoring history";
  for (int i = 0; i < history_.size(); ++i) {
    history_[i]->FromProto(state.history(i));
  }
}

template <typename Dtype>
void NesterovSolver<Dtype>::ComputeUpdateValue() {
  vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params();
  vector<float>& net_params_lr = this->net_->params_lr();
  vector<float>& net_params_weight_decay = this->net_->params_weight_decay();
  // get the learning rate
  Dtype rate = this->GetLearningRate();
  if (this->param_.display() && this->iter_ % this->param_.display() == 0) {
    LOG(INFO) << "Iteration " << this->iter_ << ", lr = " << rate;
  }
  Dtype momentum = this->param_.momentum();
  Dtype weight_decay = this->param_.weight_decay();
  string regularization_type = this->param_.regularization_type();
  switch (Caffe::mode()) {
  case Caffe::CPU:
    for (int param_id = 0; param_id < net_params.size(); ++param_id) {
      // save history momentum for stepping back
      caffe_copy(net_params[param_id]->count(),
          this->history_[param_id]->cpu_data(),
          this->update_[param_id]->mutable_cpu_data());

      Dtype local_rate = rate * net_params_lr[param_id];
      Dtype local_decay = weight_decay * net_params_weight_decay[param_id];

      if (local_decay) {
        if (regularization_type == "L2") {
          // add weight decay
          caffe_axpy(net_params[param_id]->count(),
              local_decay,
              net_params[param_id]->cpu_data(),
              net_params[param_id]->mutable_cpu_diff());
        } else if (regularization_type == "L1") {
          caffe_cpu_sign(net_params[param_id]->count(),
              net_params[param_id]->cpu_data(),
              this->temp_[param_id]->mutable_cpu_data());
          caffe_axpy(net_params[param_id]->count(),
              local_decay,
              this->temp_[param_id]->cpu_data(),
              net_params[param_id]->mutable_cpu_diff());
        } else {
          LOG(FATAL) << "Unknown regularization type: " << regularization_type;
        }
      }

      // update history
      caffe_cpu_axpby(net_params[param_id]->count(), local_rate,
                net_params[param_id]->cpu_diff(), momentum,
                this->history_[param_id]->mutable_cpu_data());

      // compute udpate: step back then over step
      caffe_cpu_axpby(net_params[param_id]->count(), Dtype(1) + momentum,
          this->history_[param_id]->cpu_data(), -momentum,
          this->update_[param_id]->mutable_cpu_data());

      // copy
      caffe_copy(net_params[param_id]->count(),
          this->update_[param_id]->cpu_data(),
          net_params[param_id]->mutable_cpu_diff());
    }
    break;
  case Caffe::GPU:
#ifndef CPU_ONLY
    for (int param_id = 0; param_id < net_params.size(); ++param_id) {
      // save history momentum for stepping back
      caffe_copy(net_params[param_id]->count(),
          this->history_[param_id]->gpu_data(),
          this->update_[param_id]->mutable_gpu_data());

      Dtype local_rate = rate * net_params_lr[param_id];
      Dtype local_decay = weight_decay * net_params_weight_decay[param_id];

      if (local_decay) {
        if (regularization_type == "L2") {
          // add weight decay
          caffe_gpu_axpy(net_params[param_id]->count(),
              local_decay,
              net_params[param_id]->gpu_data(),
              net_params[param_id]->mutable_gpu_diff());
        } else if (regularization_type == "L1") {
          caffe_gpu_sign(net_params[param_id]->count(),
              net_params[param_id]->gpu_data(),
              this->temp_[param_id]->mutable_gpu_data());
          caffe_gpu_axpy(net_params[param_id]->count(),
              local_decay,
              this->temp_[param_id]->gpu_data(),
              net_params[param_id]->mutable_gpu_diff());
        } else {
          LOG(FATAL) << "Unknown regularization type: " << regularization_type;
        }
      }

      // update history
      caffe_gpu_axpby(net_params[param_id]->count(), local_rate,
                net_params[param_id]->gpu_diff(), momentum,
                this->history_[param_id]->mutable_gpu_data());

      // compute udpate: step back then over step
      caffe_gpu_axpby(net_params[param_id]->count(), Dtype(1) + momentum,
          this->history_[param_id]->gpu_data(), -momentum,
          this->update_[param_id]->mutable_gpu_data());

      // copy
      caffe_copy(net_params[param_id]->count(),
          this->update_[param_id]->gpu_data(),
          net_params[param_id]->mutable_gpu_diff());
    }
#else
    NO_GPU;
#endif
    break;
  default:
    LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
  }
}

template <typename Dtype>
void AdaGradSolver<Dtype>::ComputeUpdateValue() {
  vector<shared_ptr<Blob<Dtype> > >& net_params = this->net_->params();
  vector<float>& net_params_lr = this->net_->params_lr();
  vector<float>& net_params_weight_decay = this->net_->params_weight_decay();
  // get the learning rate
  Dtype rate = this->GetLearningRate();
  Dtype delta = this->param_.delta();
  if (this->param_.display() && this->iter_ % this->param_.display() == 0) {
    LOG(INFO) << "Iteration " << this->iter_ << ", lr = " << rate;
  }
  Dtype weight_decay = this->param_.weight_decay();
  string regularization_type = this->param_.regularization_type();
  switch (Caffe::mode()) {
  case Caffe::CPU:
    for (int param_id = 0; param_id < net_params.size(); ++param_id) {
      Dtype local_rate = rate * net_params_lr[param_id];
      Dtype local_decay = weight_decay * net_params_weight_decay[param_id];

      if (local_decay) {
        if (regularization_type == "L2") {
          // add weight decay
          caffe_axpy(net_params[param_id]->count(),
              local_decay,
              net_params[param_id]->cpu_data(),
              net_params[param_id]->mutable_cpu_diff());
        } else if (regularization_type == "L1") {
          caffe_cpu_sign(net_params[param_id]->count(),
              net_params[param_id]->cpu_data(),
              this->temp_[param_id]->mutable_cpu_data());
          caffe_axpy(net_params[param_id]->count(),
              local_decay,
              this->temp_[param_id]->cpu_data(),
              net_params[param_id]->mutable_cpu_diff());
        } else {
          LOG(FATAL) << "Unknown regularization type: " << regularization_type;
        }
      }

      // compute square of gradient in update
      caffe_powx(net_params[param_id]->count(),
          net_params[param_id]->cpu_diff(), Dtype(2),
          this->update_[param_id]->mutable_cpu_data());

      // update history
      caffe_add(net_params[param_id]->count(),
          this->update_[param_id]->cpu_data(),
          this->history_[param_id]->cpu_data(),
          this->history_[param_id]->mutable_cpu_data());

      // prepare update
      caffe_powx(net_params[param_id]->count(),
                this->history_[param_id]->cpu_data(), Dtype(0.5),
                this->update_[param_id]->mutable_cpu_data());

      caffe_add_scalar(net_params[param_id]->count(),
                delta, this->update_[param_id]->mutable_cpu_data());

      caffe_div(net_params[param_id]->count(),
                net_params[param_id]->cpu_diff(),
                this->update_[param_id]->cpu_data(),
                this->update_[param_id]->mutable_cpu_data());

      // scale and copy
      caffe_cpu_axpby(net_params[param_id]->count(), local_rate,
          this->update_[param_id]->cpu_data(), Dtype(0),
          net_params[param_id]->mutable_cpu_diff());
    }
    break;
  case Caffe::GPU:
#ifndef CPU_ONLY
    for (int param_id = 0; param_id < net_params.size(); ++param_id) {
      Dtype local_rate = rate * net_params_lr[param_id];
      Dtype local_decay = weight_decay * net_params_weight_decay[param_id];

      if (local_decay) {
        if (regularization_type == "L2") {
          // add weight decay
          caffe_gpu_axpy(net_params[param_id]->count(),
              local_decay,
              net_params[param_id]->gpu_data(),
              net_params[param_id]->mutable_gpu_diff());
        } else if (regularization_type == "L1") {
          caffe_gpu_sign(net_params[param_id]->count(),
              net_params[param_id]->gpu_data(),
              this->temp_[param_id]->mutable_gpu_data());
          caffe_gpu_axpy(net_params[param_id]->count(),
              local_decay,
              this->temp_[param_id]->gpu_data(),
              net_params[param_id]->mutable_gpu_diff());
        } else {
          LOG(FATAL) << "Unknown regularization type: " << regularization_type;
        }
      }

      // compute square of gradient in update
      caffe_gpu_powx(net_params[param_id]->count(),
          net_params[param_id]->gpu_diff(), Dtype(2),
          this->update_[param_id]->mutable_gpu_data());

      // update history
      caffe_gpu_add(net_params[param_id]->count(),
          this->update_[param_id]->gpu_data(),
          this->history_[param_id]->gpu_data(),
          this->history_[param_id]->mutable_gpu_data());

      // prepare update
      caffe_gpu_powx(net_params[param_id]->count(),
                this->history_[param_id]->gpu_data(), Dtype(0.5),
                this->update_[param_id]->mutable_gpu_data());

      caffe_gpu_add_scalar(net_params[param_id]->count(),
                delta, this->update_[param_id]->mutable_gpu_data());

      caffe_gpu_div(net_params[param_id]->count(),
                net_params[param_id]->gpu_diff(),
                this->update_[param_id]->gpu_data(),
                this->update_[param_id]->mutable_gpu_data());

      // scale and copy
      caffe_gpu_axpby(net_params[param_id]->count(), local_rate,
          this->update_[param_id]->gpu_data(), Dtype(0),
          net_params[param_id]->mutable_gpu_diff());
    }
#else
    NO_GPU;
#endif
    break;
  default:
    LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
  }
}

INSTANTIATE_CLASS(Solver);
INSTANTIATE_CLASS(SGDSolver);
INSTANTIATE_CLASS(NesterovSolver);
INSTANTIATE_CLASS(AdaGradSolver);

} // namespace caffe
