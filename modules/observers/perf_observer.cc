#include "observers/perf_observer.h"
#include "observers/observer_config.h"
#ifndef C10_MOBILE
#include "caffe2/core/flags.h"
#include "observers/net_observer_reporter_print.h"
#endif

#include <random>
#include <time.h>
#include "caffe2/core/common.h"
#include "caffe2/core/init.h"
#include "caffe2/core/operator.h"

#ifndef C10_MOBILE
C10_DEFINE_int64(
    aiBench_netInitSampleRate,
    0,
    "One in N sampling rate for net delay");

C10_DEFINE_int64(
    aiBench_netFollowupSampleRate,
    0,
    "One in N sampling rate for net delay");

C10_DEFINE_int64(
    aiBench_netFollowupSampleCount,
    0,
    "control the following c logs");

C10_DEFINE_int64(
    aiBench_operatorNetSampleRatio,
    0,
    "One in N sampling rate for operator delay");

C10_DEFINE_int64(
    aiBench_skipIters,
    0,
    "skip the first N iterations of the net run");
#endif

namespace caffe2 {
namespace {

bool registerGlobalPerfNetObserverCreator(int* /*pargc*/, char*** /*pargv*/) {
  AddGlobalNetObserverCreator([](NetBase* subject) {
    return caffe2::make_unique<PerfNetObserver>(subject);
  });

#if !defined(C10_MOBILE)
  // for aibench usage
  caffe2::ObserverConfig::setReporter(
      caffe2::make_unique<caffe2::NetObserverReporterPrint>());

  caffe2::ObserverConfig::initSampleRate(
      FLAGS_aiBench_netInitSampleRate,
      FLAGS_aiBench_netFollowupSampleRate,
      FLAGS_aiBench_netFollowupSampleCount,
      FLAGS_aiBench_operatorNetSampleRatio,
      FLAGS_aiBench_skipIters);
#endif

  return true;
}
} // namespace

double getClockTimeMilliseconds(clockid_t clk_id) {
  int result;
  struct timespec tp;
  result = clock_gettime(clk_id, &tp);
  if (result == -1) {
    return 0.0;
  } else {
    return tp.tv_sec * 1000.0 + tp.tv_nsec / 1000000.0;
  }
}

REGISTER_CAFFE2_EARLY_INIT_FUNCTION(
    registerGlobalPerfNetObserverCreator,
    &registerGlobalPerfNetObserverCreator,
    "Caffe2 net global observer creator");

PerfNetObserver::PerfNetObserver(NetBase* subject_)
    : NetObserver(subject_), numRuns_(0) {}

PerfNetObserver::~PerfNetObserver() {}

void PerfNetObserver::Start() {
  static int visitCount = 0;
  // Select whether to log the operator or the net.
  // We have one sample rate for the entire app.
  int netInitSampleRate = ObserverConfig::getNetInitSampleRate();
  int netFollowupSampleRate = ObserverConfig::getNetFollowupSampleRate();
  int netFollowupSampleCount = ObserverConfig::getNetFollowupSampleCount();
  int operatorNetSampleRatio = ObserverConfig::getOpoeratorNetSampleRatio();
  int skipIters = ObserverConfig::getSkipIters();
  int sampleRate = visitCount > 0 ? netFollowupSampleRate : netInitSampleRate;
  if (skipIters <= numRuns_ && sampleRate > 0 && rand() % sampleRate == 0) {
    visitCount++;
    if (visitCount == netFollowupSampleCount) {
      visitCount = 0;
    }
    if (operatorNetSampleRatio > 0 && rand() % operatorNetSampleRatio == 0) {
      logType_ = PerfNetObserver::OPERATOR_DELAY;
    } else {
      logType_ = PerfNetObserver::NET_DELAY;
    }
  } else {
    logType_ = PerfNetObserver::NONE;
  }
  numRuns_++;

  if (logType_ == PerfNetObserver::OPERATOR_DELAY) {
    /* Always recreate new operator  observers
       whenever we measure operator delay */
    const auto& operators = subject_->GetOperators();
    for (auto* op : operators) {
      observerMap_[op] = op->AttachObserver(
          caffe2::make_unique<PerfOperatorObserver>(op, this));
    }
  }

  wallMilliseconds_ = getClockTimeMilliseconds(CLOCK_MONOTONIC);
  cpuMilliseconds_ = getClockTimeMilliseconds(CLOCK_PROCESS_CPUTIME_ID);
}

void PerfNetObserver::Stop() {
  if (logType_ == PerfNetObserver::NONE) {
    return;
  }
  std::map<std::string, PerformanceInformation> info;
  PerformanceInformation net_perf;
  net_perf.cpuMilliseconds =
      getClockTimeMilliseconds(CLOCK_PROCESS_CPUTIME_ID) - cpuMilliseconds_;
  net_perf.latency =
      getClockTimeMilliseconds(CLOCK_MONOTONIC) - wallMilliseconds_;

  if (logType_ == PerfNetObserver::OPERATOR_DELAY) {
    const auto& operators = subject_->GetOperators();
    for (int idx = 0; idx < operators.size(); ++idx) {
      const auto* op = operators[idx];
      auto name = getObserverName(op, idx);
      PerformanceInformation p;
      const PerfOperatorObserver* opObserver =
          static_cast<const PerfOperatorObserver*>(observerMap_[op]);
      p.latency = opObserver->getWallMilliseconds();
      p.cpuMilliseconds = opObserver->getCpuMilliseconds();
      p.engine = op->engine();
      p.type = op->type();
      p.tensor_shapes =
          static_cast<const PerfOperatorObserver*>(observerMap_[op])
              ->getTensorShapes();

      if (op->has_debug_def()) {
        for (auto arg : op->debug_def().arg()) {
          p.args.emplace_back(arg);
        }
      }

      info.insert({name, p});
    }

    /* clear all operator delay after use so that we don't spent time
       collecting the operator delay info in later runs */
    for (auto* op : operators) {
      op->DetachObserver(observerMap_[op]);
    }
    observerMap_.clear();
  }
  info.insert({"NET_DELAY", net_perf});
  ObserverConfig::getReporter()->report(subject_, info);
}

caffe2::string PerfNetObserver::getObserverName(const OperatorBase* op, int idx)
    const {
  string opType = op->has_debug_def() ? op->debug_def().type() : "NO_TYPE";
  string displayName =
      (op->has_debug_def() ? op->debug_def().name().size()
               ? op->debug_def().name()
               : (op->debug_def().output_size() ? op->debug_def().output(0)
                                                : "NO_OUTPUT")
                           : "NO_DEF");
  caffe2::string name =
      "ID_" + c10::to_string(idx) + "_" + opType + "_" + displayName;
  return name;
}

PerfOperatorObserver::PerfOperatorObserver(
    OperatorBase* op,
    PerfNetObserver* netObserver)
    : ObserverBase<OperatorBase>(op),
      netObserver_(netObserver),
      wallMilliseconds_(0),
      cpuMilliseconds_(0) {
  CAFFE_ENFORCE(netObserver_, "Observers can't operate outside of the net");
}

PerfOperatorObserver::~PerfOperatorObserver() {}

void PerfOperatorObserver::Start() {
  wallMilliseconds_ = getClockTimeMilliseconds(CLOCK_MONOTONIC);
  cpuMilliseconds_ = getClockTimeMilliseconds(CLOCK_PROCESS_CPUTIME_ID);
}

void PerfOperatorObserver::Stop() {
  /* Time from the start of the net minus the time spent on all other
     operators is the time spent on this operator */
  cpuMilliseconds_ =
      getClockTimeMilliseconds(CLOCK_PROCESS_CPUTIME_ID) - cpuMilliseconds_;
  wallMilliseconds_ =
      getClockTimeMilliseconds(CLOCK_MONOTONIC) - wallMilliseconds_;
  tensor_shapes_ = subject_->InputTensorShapes();
}

double PerfOperatorObserver::getWallMilliseconds() const {
  return wallMilliseconds_;
}

double PerfOperatorObserver::getCpuMilliseconds() const {
  return cpuMilliseconds_;
}

std::vector<TensorShape> PerfOperatorObserver::getTensorShapes() const {
  return tensor_shapes_;
}

} // namespace caffe2
