// aosp/hardware/interfaces/ir/aidl/default

/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <aidl/android/hardware/ir/BnConsumerIr.h>
#include <aidl/android/hardware/ir/ConsumerIrFreqRange.h>
#include <aidl/android/hardware/ir/IConsumerIrCallback.h>
#include <aidl/android/hardware/ir/IrEvent.h>

#include <android-base/file.h>        // WriteStringToFile, ReadFileToString
#include <android-base/logging.h>
#include <android-base/properties.h>  // GetProperty

#include <android/binder_interface_utils.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <hardware/consumerir.h>
#include <log/log.h>

#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <numeric>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <time.h>

using ::aidl::android::hardware::ir::ConsumerIrFreqRange;
using ::aidl::android::hardware::ir::IConsumerIrCallback;
using ::aidl::android::hardware::ir::IrEvent;

namespace {

// LÊ PROPRIEDADES (persist.* com fallback p/ ro.*)
static std::string getProp(const char* pPersist, const char* pRo, const char* defv) {
    auto v = android::base::GetProperty(pPersist, "");
    if (!v.empty()) return v;
    v = android::base::GetProperty(pRo, "");
    return v.empty() ? defv : v;
}

std::string getMode() {
    // cli | sysfs | legacy
    return getProp("persist.vendor.ir.mode", "ro.vendor.ir.mode", "cli");
}
std::string getSysfsPath() {
    return getProp("persist.vendor.ir.sysfs", "ro.vendor.ir.sysfs",
                   "/sys/kernel/infrared/transmit");
}
std::string getSysfsFormat() {
    return getProp("persist.vendor.ir.sysfs_format", "ro.vendor.ir.sysfs_format", "raw");
}

// Monta um payload textual simples: FREQ=...;LEN=...;DATA=a,b,c,...
std::string buildPayload(int32_t carrierHz, const std::vector<int32_t>& pattern) {
    std::ostringstream os;
    os << "FREQ=" << carrierHz << ";LEN=" << pattern.size() << ";DATA=";
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (i) os << ",";
        os << pattern[i];
    }
    return os.str();
}

// Ponte para compat: "NEC 10C8E11E" (pega de properties)
static std::string buildProtoHexPayload() {
    auto proto = getProp("persist.vendor.ir.proto", "ro.vendor.ir.proto", "NEC");
    auto hex   = getProp("persist.vendor.ir.hex",   "ro.vendor.ir.hex",   "10C8E11E");
    return proto + " " + hex; // ex.: "NEC 10C8E11E"
}

// Grava payloads de depuração (garante diretório e ignora erro)
static void writeLastCmdDebug(const std::string& payload) {
    constexpr const char* kDir = "/data/vendor/ir";
    constexpr const char* kOut = "/data/vendor/ir/last_cmd";
    (void)::android::base::WriteStringToFile("1", std::string(kDir) + "/.keep"); // cria dir
    (void)::android::base::WriteStringToFile(payload, kOut);
}

// Caminho sysfs de recepção (driver DevTitans)
static constexpr const char* kSysfsRx = "/sys/kernel/infrared/receive";

} // namespace

namespace aidl::android::hardware::ir {

class ConsumerIr : public BnConsumerIr {
  public:
    ConsumerIr();
    ~ConsumerIr() override;

  private:
    // Métodos da AIDL
    ::ndk::ScopedAStatus getCarrierFreqs(
            std::vector<ConsumerIrFreqRange>* _aidl_return) override;

    ::ndk::ScopedAStatus transmit(
            int32_t in_carrierFreqHz,
            const std::vector<int32_t>& in_pattern) override;

    // NOVOS MÉTODOS V2 (RX)
    ::ndk::ScopedAStatus setReceiveEnabled(bool in_enable) override;

    ::ndk::ScopedAStatus registerCallback(
            const std::shared_ptr<IConsumerIrCallback>& in_cb) override;

    ::ndk::ScopedAStatus unregisterCallback(
            const std::shared_ptr<IConsumerIrCallback>& in_cb) override;

    // Implementação de RX
    void rxLoop();
    bool parseRxLine(const std::string& line, IrEvent* ev);

  private:
    // Estado TX legacy/sysfs/cli
    consumerir_device_t* mDevice = nullptr;
    std::string mMode;
    std::string mSysfsPath;
    std::string mSysfsFormat;

    // Estado RX
    std::thread mRxThread;
    std::atomic<bool> mRun{true};
    std::atomic<bool> mRxEnabled{false};
    std::mutex mCbMutex;
    std::vector<std::shared_ptr<IConsumerIrCallback>> mCallbacks;
};

ConsumerIr::ConsumerIr()
  : mMode(getMode()),
    mSysfsPath(getSysfsPath()),
    mSysfsFormat(getSysfsFormat()) {

    ALOGI("IR HAL starting. mode=%s sysfs=%s fmt=%s",
          mMode.c_str(), mSysfsPath.c_str(), mSysfsFormat.c_str());

    // Backend legacy (HIDL antigo)
    if (mMode == "legacy") {
        const hw_module_t* hw_module = nullptr;
        int ret = hw_get_module(CONSUMERIR_HARDWARE_MODULE_ID, &hw_module);
        if (ret != 0) {
            ALOGE("hw_get_module %s failed: %d", CONSUMERIR_HARDWARE_MODULE_ID, ret);
        } else {
            ret = hw_module->methods->open(
                    hw_module, CONSUMERIR_TRANSMITTER, (hw_device_t**)&mDevice);
            if (ret < 0) {
                ALOGE("Can't open consumer IR transmitter, error: %d", ret);
                mDevice = nullptr;
            }
        }
    }

    // Thread de recepção (driver DevTitans via sysfs)
    mRxThread = std::thread([this] { rxLoop(); });
}

ConsumerIr::~ConsumerIr() {
    mRun.store(false);
    if (mRxThread.joinable()) {
        mRxThread.join();
    }
}

::ndk::ScopedAStatus ConsumerIr::getCarrierFreqs(
        std::vector<ConsumerIrFreqRange>* _aidl_return) {
    // Se tiver device legacy, usa a tabela dele
    if (mDevice) {
        int32_t len = mDevice->get_num_carrier_freqs(mDevice);
        if (len > 0) {
            std::unique_ptr<consumerir_freq_range_t[]> rangeAr(
                    new consumerir_freq_range_t[len]);
            bool success =
                    (mDevice->get_carrier_freqs(mDevice, len, rangeAr.get()) >= 0);
            if (success) {
                _aidl_return->resize(len);
                for (int32_t i = 0; i < len; i++) {
                    (*_aidl_return)[i].minHz = static_cast<uint32_t>(rangeAr[i].min);
                    (*_aidl_return)[i].maxHz = static_cast<uint32_t>(rangeAr[i].max);
                }
                return ::ndk::ScopedAStatus::ok();
            }
        }
    }

    // Fallback: publicar uma faixa padrão para os apps aceitarem
    _aidl_return->clear();
    ConsumerIrFreqRange r;
    r.minHz = 30000; // 30 kHz
    r.maxHz = 60000; // 60 kHz
    _aidl_return->push_back(r);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus ConsumerIr::transmit(
        int32_t in_carrierFreqHz,
        const std::vector<int32_t>& in_pattern) {
    if (in_carrierFreqHz <= 0) {
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (in_pattern.empty()) {
        ALOGE("IR transmit: padrao invalido (size=%zu)", in_pattern.size());
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    constexpr size_t kMaxElems = 2048;
    if (in_pattern.size() > kMaxElems) {
        ALOGE("IR transmit: padrao muito grande (%zu > %zu)",
              in_pattern.size(), kMaxElems);
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    ALOGI("IR transmit: freq=%d Hz, len=%zu (mode=%s)",
          in_carrierFreqHz, in_pattern.size(), mMode.c_str());

    if (mMode == "cli") {
        const auto payload = buildPayload(in_carrierFreqHz, in_pattern);
        writeLastCmdDebug(payload);
        ALOGI("IR transmit (CLI): %s", payload.c_str());
        return ::ndk::ScopedAStatus::ok();
    }

    if (mMode == "sysfs") {
        std::ostringstream cmd;
        cmd << in_carrierFreqHz << " ";
        for (size_t i = 0; i < in_pattern.size(); ++i) {
            if (i) cmd << ",";
            cmd << in_pattern[i];
        }
        cmd << "\n";

        // grava no sysfs mas ignora erro de retorno (muitos drivers retornam -EIO mesmo tendo transmitido)
        ::android::base::WriteStringToFile(cmd.str(), mSysfsPath);
        ALOGI("IR transmit (SYSFS) enviado -> %s :: %s",
              mSysfsPath.c_str(), cmd.str().c_str());

        return ::ndk::ScopedAStatus::ok();
    }

    if (mDevice) {
        mDevice->transmit(mDevice, in_carrierFreqHz,
                          in_pattern.data(), in_pattern.size());
        return ::ndk::ScopedAStatus::ok();
    }

    ALOGE("IR transmit: nenhum backend disponivel (mode=%s)", mMode.c_str());
    return ::ndk::ScopedAStatus::fromServiceSpecificError(-2 /*vendor*/);
}

// =====================
//    MÉTODOS DE RX
// =====================

::ndk::ScopedAStatus ConsumerIr::setReceiveEnabled(bool in_enable) {
    mRxEnabled.store(in_enable);
    ALOGI("IR HAL: RX %s", in_enable ? "ON" : "OFF");
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus ConsumerIr::registerCallback(
        const std::shared_ptr<IConsumerIrCallback>& in_cb) {
    if (!in_cb) {
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard<std::mutex> lk(mCbMutex);

    auto newBinder = in_cb->asBinder().get();
    for (auto& c : mCallbacks) {
        if (c && c->asBinder().get() == newBinder) {
            // Já registrado
            return ::ndk::ScopedAStatus::ok();
        }
    }
    mCallbacks.push_back(in_cb);
    ALOGI("IR HAL: callback registrado. total=%zu", mCallbacks.size());
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus ConsumerIr::unregisterCallback(
        const std::shared_ptr<IConsumerIrCallback>& in_cb) {
    if (!in_cb) {
        return ::ndk::ScopedAStatus::ok();
    }
    std::lock_guard<std::mutex> lk(mCbMutex);
    auto binder = in_cb->asBinder().get();

    mCallbacks.erase(
        std::remove_if(
            mCallbacks.begin(), mCallbacks.end(),
            [&](const std::shared_ptr<IConsumerIrCallback>& c) {
                return !c || c->asBinder().get() == binder;
            }),
        mCallbacks.end());

    ALOGI("IR HAL: callback removido. total=%zu", mCallbacks.size());
    return ::ndk::ScopedAStatus::ok();
}

void ConsumerIr::rxLoop() {
    int fd = ::open(kSysfsRx, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        ALOGE("IR HAL: open(%s) failed: %m", kSysfsRx);
        return;
    }

    char buf[4096];

    for (;;) {
        if (!mRun.load()) break;

        if (!mRxEnabled.load()) {
            usleep(100000); // 100 ms
            continue;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = ::poll(&pfd, 1, 1000); // timeout 1s
        if (pr <= 0) continue;

        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = '\0';

        // reposiciona para próxima leitura
        (void)::lseek(fd, 0, SEEK_SET);

        IrEvent ev{};
        if (!parseRxLine(std::string(buf), &ev)) {
            continue;
        }

        std::vector<std::shared_ptr<IConsumerIrCallback>> cbs;
        {
            std::lock_guard<std::mutex> lk(mCbMutex);
            cbs = mCallbacks;
        }
        for (auto& cb : cbs) {
            if (cb) {
                (void)cb->onEvent(ev);
            }
        }
    }

    ::close(fd);
}

bool ConsumerIr::parseRxLine(const std::string& line, IrEvent* ev) {
    if (!ev) return false;

    // Timestamp baseado no tempo de boot
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
        ev->timestampNanos =
                static_cast<int64_t>(ts.tv_sec) * 1000000000LL +
                static_cast<int64_t>(ts.tv_nsec);
    } else {
        ev->timestampNanos = 0;
    }

    // Exemplo simples:
    //  - string começando com "NEC " => protocolo NEC fixo
    //  - caso contrário, trata como RAW genérico
    if (line.rfind("NEC ", 0) == 0) {
        ev->protocol = "NEC";
        ev->carrierFrequencyHz = 38000;
        ev->address = -1;
        ev->command = -1;
        // Aqui você pode decodificar o hex se o driver expuser algo como "NEC 10C8E11E"
        return true;
    }

    // RAW genérico
    ev->protocol.clear();
    ev->carrierFrequencyHz = 38000;
    ev->address = -1;
    ev->command = -1;
    // Se quiser, parseia line do tipo "FREQ=38000;RAW=9000,4500,..."
    // e preenche ev->rawPattern.
    return true;
}

}  // namespace aidl::android::hardware::ir

using aidl::android::hardware::ir::ConsumerIr;

int main() {
    auto binder = ::ndk::SharedRefBase::make<ConsumerIr>();
    const std::string name = std::string() + ConsumerIr::descriptor + "/default";
    CHECK_EQ(STATUS_OK,
             AServiceManager_addService(binder->asBinder().get(), name.c_str()))
            << "Failed to register " << name;

    ABinderProcess_setThreadPoolMaxThreadCount(0);
    ABinderProcess_joinThreadPool();

    return EXIT_FAILURE;  // should not be reached
}
