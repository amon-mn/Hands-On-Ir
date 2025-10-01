# Implementação de Emissores de Infravermelho no AOSP

**Autor:** \[Equipe 4 - Infravermelho\]
**Data:** \[26/09/2025\]

------------------------------------------------------------------------

## 1. Resumo da Arquitetura *ConsumerIr* do AOSP

A arquitetura do emissor de infravermelho (IR) no Android Open Source Project (AOSP) é um modelo em camadas que possibilita que aplicações de alto nível controlem o hardware de forma padronizada. O sistema utiliza a *Hardware Abstraction Layer* (HAL) para desacoplar o framework do software do hardware específico de cada dispositivo, garantindo portabilidade e modularidade.

### Fluxo de Dados da Transmissão IR

- **Camada de Aplicação**: Um aplicativo de controle remoto inicia a transmissão, consumindo a API padrão do Android.  
- **Camada de Framework**: A API `ConsumerIrManager` processa a solicitação do aplicativo e faz uma chamada de procedimento remoto (RPC) para o serviço de sistema.  
- **Camada de Serviço de Sistema**: O `ConsumerIrService` recebe a solicitação, valida os dados e se comunica com a camada nativa via *Java Native Interface* (JNI).  
- **Camada de Abstração de Hardware (HAL)**: A HAL traduz a chamada do framework para uma interface de dispositivo de baixo nível, preparando os dados para o kernel.  
- **Camada de Kernel**: O driver interpreta as instruções da HAL e as envia para o hardware, orquestrando a emissão do sinal.  


------------------------------------------------------------------------

## 2. Localização e Análise dos Códigos-Fonte

Esta seção apresenta os arquivos analisados que foram fundamentais para compreender a arquitetura.

### 2.1. Camada de Framework: `ConsumerIrManager.java`

- **Função**: Atua como a API pública e principal ponto de entrada para o controle de IR no Android.  
- **Principais Métodos**: Expõe métodos essenciais como `transmit()` e `getCarrierFrequencies()`.  
- **Análise**: A classe não interage diretamente com o hardware. Em vez disso, ela se conecta ao serviço de sistema `IConsumerIrService` para delegar a tarefa via RPC, o que isola o aplicativo da complexidade do hardware.  


``` java
package android.hardware;

import android.annotation.RequiresFeature;
import android.annotation.SystemService;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.ServiceManager.ServiceNotFoundException;
import android.util.Log;

/**
 * Class that operates consumer infrared on the device.
 */
@SystemService(Context.CONSUMER_IR_SERVICE)
@RequiresFeature(PackageManager.FEATURE_CONSUMER_IR)
public final class ConsumerIrManager {
    private static final String TAG = "ConsumerIr";

    private final String mPackageName;
    private final IConsumerIrService mService;

    /**
     * @hide to prevent subclassing from outside of the framework
     */
    public ConsumerIrManager(Context context) throws ServiceNotFoundException {
        mPackageName = context.getPackageName();
        mService = IConsumerIrService.Stub.asInterface(
                ServiceManager.getServiceOrThrow(Context.CONSUMER_IR_SERVICE));
    }

    /**
     * Check whether the device has an infrared emitter.
     *
     * @return true if the device has an infrared emitter, else false.
     */
    public boolean hasIrEmitter() {
        if (mService == null) {
            Log.w(TAG, "no consumer ir service.");
            return false;
        }

        try {
            return mService.hasIrEmitter();
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /**
     * Transmit an infrared pattern
     * <p>
     * This method is synchronous; when it returns the pattern has
     * been transmitted. Only patterns shorter than 2 seconds will
     * be transmitted.
     * </p>
     *
     * @param carrierFrequency The IR carrier frequency in Hertz.
     * @param pattern The alternating on/off pattern in microseconds to transmit.
     */
    public void transmit(int carrierFrequency, int[] pattern) {
        if (mService == null) {
            Log.w(TAG, "failed to transmit; no consumer ir service.");
            return;
        }

        try {
            mService.transmit(mPackageName, carrierFrequency, pattern);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /**
     * Represents a range of carrier frequencies (inclusive) on which the
     * infrared transmitter can transmit
     */
    public final class CarrierFrequencyRange {
        private final int mMinFrequency;
        private final int mMaxFrequency;

        /**
         * Create a segment of a carrier frequency range.
         *
         * @param min The minimum transmittable frequency in this range segment.
         * @param max The maximum transmittable frequency in this range segment.
         */
        public CarrierFrequencyRange(int min, int max) {
            mMinFrequency = min;
            mMaxFrequency = max;
        }

        /**
         * Get the minimum (inclusive) frequency in this range segment.
         */
        public int getMinFrequency() {
            return mMinFrequency;
        }

        /**
         * Get the maximum (inclusive) frequency in this range segment.
         */
        public int getMaxFrequency() {
            return mMaxFrequency;
        }
    };

    /**
     * Query the infrared transmitter's supported carrier frequencies
     *
     * @return an array of
     * {@link android.hardware.ConsumerIrManager.CarrierFrequencyRange}
     * objects representing the ranges that the transmitter can support, or
     * null if there was an error communicating with the Consumer IR Service.
     */
    public CarrierFrequencyRange[] getCarrierFrequencies() {
        if (mService == null) {
            Log.w(TAG, "no consumer ir service.");
            return null;
        }

        try {
            int[] freqs = mService.getCarrierFrequencies();
            if (freqs.length % 2 != 0) {
                Log.w(TAG, "consumer ir service returned an uneven number of frequencies.");
                return null;
            }
            CarrierFrequencyRange[] range = new CarrierFrequencyRange[freqs.length / 2];

            for (int i = 0; i < freqs.length; i += 2) {
                range[i / 2] = new CarrierFrequencyRange(freqs[i], freqs[i+1]);
            }
            return range;
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }
}
```

------------------------------------------------------------------------

### 2.2. Camada de Serviço de Sistema: `IConsumerIrService.aidl` e `ConsumerIrService.java`

- **Função**: O `IConsumerIrService.aidl` define o contrato de comunicação entre o framework e o serviço do sistema.  
- **Implementação**: A classe `ConsumerIrService.java` implementa esse contrato. Ela recebe as solicitações, executa validações de segurança e faz a ponte para a camada nativa, chamando métodos JNI.  
- **Análise**: Este serviço atua como um intermediário crítico, convertendo as solicitações do framework em chamadas de nível inferior, como `halTransmit` e `halGetCarrierFrequencies`.  

``` java
package android.hardware;

/** {@hide} */
interface IConsumerIrService
{
    @RequiresNoPermission
    boolean hasIrEmitter();

    @EnforcePermission("TRANSMIT_IR")
    void transmit(String packageName, int carrierFrequency, in int[] pattern);

    @EnforcePermission("TRANSMIT_IR")
    int[] getCarrierFrequencies();
}
```

``` java
package com.android.server;

import static android.Manifest.permission.TRANSMIT_IR;

import android.annotation.EnforcePermission;
import android.annotation.RequiresNoPermission;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.IConsumerIrService;
import android.hardware.ir.ConsumerIrFreqRange;
import android.hardware.ir.IConsumerIr;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Slog;

import com.android.server.utils.LazyJniRegistrar;

public class ConsumerIrService extends IConsumerIrService.Stub {
    private static final String TAG = "ConsumerIrService";

    private static final int MAX_XMIT_TIME = 2000000; /* in microseconds */

    private static native boolean getHidlHalService();
    private static native int halTransmit(int carrierFrequency, int[] pattern);
    private static native int[] halGetCarrierFrequencies();

    static {
        LazyJniRegistrar.registerConsumerIrService();
    }

    private final Context mContext;
    private final PowerManager.WakeLock mWakeLock;
    private final boolean mHasNativeHal;
    private final Object mHalLock = new Object();
    private IConsumerIr mAidlService = null;

    ConsumerIrService(Context context) {
        mContext = context;
        PowerManager pm = (PowerManager)context.getSystemService(
                Context.POWER_SERVICE);
        mWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, TAG);
        mWakeLock.setReferenceCounted(true);

        mHasNativeHal = getHalService();

        if (mContext.getPackageManager().hasSystemFeature(PackageManager.FEATURE_CONSUMER_IR)) {
            if (!mHasNativeHal) {
                throw new RuntimeException("FEATURE_CONSUMER_IR present, but no IR HAL loaded!");
            }
        } else if (mHasNativeHal) {
            throw new RuntimeException("IR HAL present, but FEATURE_CONSUMER_IR is not set!");
        }
    }

    @Override
    @RequiresNoPermission
    public boolean hasIrEmitter() {
        return mHasNativeHal;
    }

    private boolean getHalService() {
        // Attempt to get the AIDL HAL service first
        final String fqName = IConsumerIr.DESCRIPTOR + "/default";
        mAidlService = IConsumerIr.Stub.asInterface(
                        ServiceManager.waitForDeclaredService(fqName));
        if (mAidlService != null) {
            return true;
        }

        // Fall back to the HIDL HAL service
        return getHidlHalService();
    }

    private void throwIfNoIrEmitter() {
        if (!mHasNativeHal) {
            throw new UnsupportedOperationException("IR emitter not available");
        }
    }


    @Override
    @EnforcePermission(TRANSMIT_IR)
    public void transmit(String packageName, int carrierFrequency, int[] pattern) {
        super.transmit_enforcePermission();

        long totalXmitTime = 0;

        for (int slice : pattern) {
            if (slice <= 0) {
                throw new IllegalArgumentException("Non-positive IR slice");
            }
            totalXmitTime += slice;
        }

        if (totalXmitTime > MAX_XMIT_TIME ) {
            throw new IllegalArgumentException("IR pattern too long");
        }

        throwIfNoIrEmitter();

        // Right now there is no mechanism to ensure fair queing of IR requests
        synchronized (mHalLock) {
            if (mAidlService != null) {
                try {
                    mAidlService.transmit(carrierFrequency, pattern);
                } catch (RemoteException ignore) {
                    Slog.e(TAG, "Error transmitting frequency: " + carrierFrequency);
                }
            } else {
                int err = halTransmit(carrierFrequency, pattern);

                if (err < 0) {
                    Slog.e(TAG, "Error transmitting: " + err);
                }
            }
        }
    }

    @Override
    @EnforcePermission(TRANSMIT_IR)
    public int[] getCarrierFrequencies() {
        super.getCarrierFrequencies_enforcePermission();

        throwIfNoIrEmitter();

        synchronized(mHalLock) {
            if (mAidlService != null) {
                try {
                    ConsumerIrFreqRange[] output = mAidlService.getCarrierFreqs();
                    if (output.length <= 0) {
                        Slog.e(TAG, "Error getting carrier frequencies.");
                    }
                    int[] result = new int[output.length * 2];
                    for (int i = 0; i < output.length; i++) {
                        result[i * 2] = output[i].minHz;
                        result[i * 2 + 1] = output[i].maxHz;
                    }
                    return result;
                } catch (RemoteException ignore) {
                    return null;
                }
            } else {
                return halGetCarrierFrequencies();
            }
        }
    }
}
```

------------------------------------------------------------------------

### 2.3. Camada de HAL

A comunicação de baixo nível com o hardware é gerenciada pela HAL, implementada em C/C++.

- **`ConsumerIrService.cpp`**: Ponte JNI, responsável por converter tipos de dados Java em tipos C++ (HIDL) e invocar o método `transmit` da HAL.  
- **`ConsumerIr.cpp`**: Implementação da HAL no formato HIDL, que atua como um *wrapper* para a implementação de nível mais baixo.  
- **`consumerir.c`**: Implementação de referência da HAL em C. A análise revelou que, em vez de interagir com um driver real, ela simula a transmissão com um atraso via `usleep()`, o que confirma que essa parte do código deve ser customizada.  

``` cpp
#define LOG_TAG "ConsumerIrService"

#include "jni.h"
#include <nativehelper/JNIHelp.h>
#include "android_runtime/AndroidRuntime.h"

#include <stdlib.h>
#include <utils/misc.h>
#include <utils/Log.h>
#include <android/hardware/ir/1.0/IConsumerIr.h>
#include <nativehelper/ScopedPrimitiveArray.h>

using ::android::hardware::ir::V1_0::IConsumerIr;
using ::android::hardware::ir::V1_0::ConsumerIrFreqRange;
using ::android::hardware::hidl_vec;

namespace android {

static sp<IConsumerIr> mHal;

static jboolean getHidlHalService(JNIEnv * /* env */, jobject /* obj */) {
    // TODO(b/31632518)
    mHal = IConsumerIr::getService();
    return mHal != nullptr;
}

static jint halTransmit(JNIEnv *env, jobject /* obj */, jint carrierFrequency,
   jintArray pattern) {
    ScopedIntArrayRO cPattern(env, pattern);
    if (cPattern.get() == NULL) {
        return -EINVAL;
    }
    hidl_vec<int32_t> patternVec;
    patternVec.setToExternal(const_cast<int32_t*>(cPattern.get()), cPattern.size());

    bool success = mHal->transmit(carrierFrequency, patternVec);
    return success ? 0 : -1;
}

static jintArray halGetCarrierFrequencies(JNIEnv *env, jobject /* obj */) {
    int len;
    hidl_vec<ConsumerIrFreqRange> ranges;
    bool success;

    auto cb = [&](bool s, hidl_vec<ConsumerIrFreqRange> vec) {
            ranges = vec;
            success = s;
    };
    mHal->getCarrierFreqs(cb);

    if (!success) {
        return NULL;
    }
    len = ranges.size();

    int i;
    ScopedIntArrayRW freqsOut(env, env->NewIntArray(len*2));
    jint *arr = freqsOut.get();
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        arr[i*2] = static_cast<jint>(ranges[i].min);
        arr[i*2+1] = static_cast<jint>(ranges[i].max);
    }

    return freqsOut.getJavaArray();
}

static const JNINativeMethod method_table[] = {
        {"getHidlHalService", "()Z", (void *)getHidlHalService},
        {"halTransmit", "(I[I)I", (void *)halTransmit},
        {"halGetCarrierFrequencies", "()[I", (void *)halGetCarrierFrequencies},
};

int register_android_server_ConsumerIrService(JNIEnv *env) {
    return jniRegisterNativeMethods(env, "com/android/server/ConsumerIrService",
            method_table, NELEM(method_table));
}

};
```

``` cpp
#define LOG_TAG "ConsumerIrService"

#include <log/log.h>

#include <hardware/hardware.h>
#include <hardware/consumerir.h>

#include "ConsumerIr.h"

namespace android {
namespace hardware {
namespace ir {
namespace V1_0 {
namespace implementation {

ConsumerIr::ConsumerIr(consumerir_device_t *device) {
    mDevice = device;
}

// Methods from ::android::hardware::consumerir::V1_0::IConsumerIr follow.
Return<bool> ConsumerIr::transmit(int32_t carrierFreq, const hidl_vec<int32_t>& pattern) {
    return mDevice->transmit(mDevice, carrierFreq, pattern.data(), pattern.size()) == 0;
}

Return<void> ConsumerIr::getCarrierFreqs(getCarrierFreqs_cb _hidl_cb) {
    int32_t len = mDevice->get_num_carrier_freqs(mDevice);
    if (len < 0) {
        _hidl_cb(false, {});
        return Void();
    }

    consumerir_freq_range_t *rangeAr = new consumerir_freq_range_t[len];
    bool success = (mDevice->get_carrier_freqs(mDevice, len, rangeAr) >= 0);
    if (!success) {
        _hidl_cb(false, {});
        return Void();
    }

    hidl_vec<ConsumerIrFreqRange> rangeVec;
    rangeVec.resize(len);
    for (int32_t i = 0; i < len; i++) {
        rangeVec[i].min = static_cast<uint32_t>(rangeAr[i].min);
        rangeVec[i].max = static_cast<uint32_t>(rangeAr[i].max);
    }
    _hidl_cb(true, rangeVec);
    return Void();
}


IConsumerIr* HIDL_FETCH_IConsumerIr(const char * /*name*/) {
    consumerir_device_t *dev;
    const hw_module_t *hw_module = NULL;

    int ret = hw_get_module(CONSUMERIR_HARDWARE_MODULE_ID, &hw_module);
    if (ret != 0) {
        ALOGE("hw_get_module %s failed: %d", CONSUMERIR_HARDWARE_MODULE_ID, ret);
        return nullptr;
    }
    ret = hw_module->methods->open(hw_module, CONSUMERIR_TRANSMITTER, (hw_device_t **) &dev);
    if (ret < 0) {
        ALOGE("Can't open consumer IR transmitter, error: %d", ret);
        return nullptr;
    }
    return new ConsumerIr(dev);
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace ir
}  // namespace hardware
}  // namespace android
```

``` c
#define LOG_TAG "ConsumerIrHal"

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>

#include <hardware/consumerir.h>
#include <hardware/hardware.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const consumerir_freq_range_t consumerir_freqs[] = {
    {.min = 30000, .max = 30000},
    {.min = 33000, .max = 33000},
    {.min = 36000, .max = 36000},
    {.min = 38000, .max = 38000},
    {.min = 40000, .max = 40000},
    {.min = 56000, .max = 56000},
};

static int consumerir_transmit(struct consumerir_device *dev __unused,
   int carrier_freq, const int pattern[], int pattern_len)
{
    int total_time = 0;
    long i;

    for (i = 0; i < pattern_len; i++)
        total_time += pattern[i];

    /* simulate the time spent transmitting by sleeping */
    ALOGD("transmit for %d uS at %d Hz", total_time, carrier_freq);
    usleep(total_time);

    return 0;
}

static int consumerir_get_num_carrier_freqs(struct consumerir_device *dev __unused)
{
    return ARRAY_SIZE(consumerir_freqs);
}

static int consumerir_get_carrier_freqs(struct consumerir_device *dev __unused,
    size_t len, consumerir_freq_range_t *ranges)
{
    size_t to_copy = ARRAY_SIZE(consumerir_freqs);

    to_copy = len < to_copy ? len : to_copy;
    memcpy(ranges, consumerir_freqs, to_copy * sizeof(consumerir_freq_range_t));
    return to_copy;
}

static int consumerir_close(hw_device_t *dev)
{
    free(dev);
    return 0;
}

/*
 * Generic device handling
 */
static int consumerir_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    if (strcmp(name, CONSUMERIR_TRANSMITTER) != 0) {
        return -EINVAL;
    }
    if (device == NULL) {
        ALOGE("NULL device on open");
        return -EINVAL;
    }

    consumerir_device_t *dev = malloc(sizeof(consumerir_device_t));
    memset(dev, 0, sizeof(consumerir_device_t));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*) module;
    dev->common.close = consumerir_close;

    dev->transmit = consumerir_transmit;
    dev->get_num_carrier_freqs = consumerir_get_num_carrier_freqs;
    dev->get_carrier_freqs = consumerir_get_carrier_freqs;

    *device = (hw_device_t*) dev;
    return 0;
}

static struct hw_module_methods_t consumerir_module_methods = {
    .open = consumerir_open,
};

consumerir_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag                = HARDWARE_MODULE_TAG,
        .module_api_version = CONSUMERIR_MODULE_API_VERSION_1_0,
        .hal_api_version    = HARDWARE_HAL_API_VERSION,
        .id                 = CONSUMERIR_HARDWARE_MODULE_ID,
        .name               = "Demo IR HAL",
        .author             = "The Android Open Source Project",
        .methods            = &consumerir_module_methods,
    },
};
```

------------------------------------------------------------------------

## 3. Padrão de Dados e a Lacuna na Comunicação com o Kernel

### 3.1. O Padrão de Dados do Sinal IR

O padrão de dados que o framework envia para a HAL é um dos insights mais importantes da nossa pesquisa. O sinal de infravermelho não é transmitido como um código hexadecimal ou uma string, mas sim como um **array de inteiros (`int[]`)**. Esse formato permite uma representação precisa do sinal, independente de qual protocolo (NEC, RC5, etc.) ele pertença.

Cada par de valores no array representa um ciclo de pulso e pausa em microssegundos:

- O **primeiro valor** de cada par (`pattern[i]`) especifica o tempo que o emissor deve permanecer **ligado**.  
- O **segundo valor** (`pattern[i+1]`) especifica o tempo que o emissor deve permanecer **desligado**.  

Essa abordagem de "pulsos e pausas" é o formato universal que a HAL precisa traduzir para o driver do kernel, que, por sua vez, o converte em pulsos elétricos no hardware.

---

### 3.2. A Comunicação com o Kernel: O Ponto de Abstração Crítico

A nossa análise mostrou que a camada HAL atua como a interface entre o software Android e o driver do kernel. Em uma implementação real, o código da HAL faria a ponte para o kernel, geralmente por meio de um arquivo de dispositivo em `/dev`.

Os métodos mais comuns para essa comunicação seriam:

- **`write()`**: utilizado para enviar o array de pulsos e pausas para o driver, que então orquestra a emissão do sinal.  
- **`ioctl()`**: utilizado para configurar parâmetros específicos do hardware que não estão relacionados à transmissão de dados, como a frequência da portadora (Hertz).  

A principal descoberta da nossa pesquisa é que a implementação de referência do AOSP (`consumerir.c`) **não possui essa comunicação**. Em vez de usar `write()` ou `ioctl()`, ela utiliza `usleep()` para simplesmente simular o tempo de transmissão. Isso valida nossa abordagem de que a implementação real é um módulo separado, específico do fabricante, que precisa ser desenvolvido.

---

## 4. Conclusão e Próximos Passos

A nossa análise robusta confirmou que a arquitetura do *ConsumerIr* no AOSP está meticulosamente definida em camadas até o nível da HAL. Todas as interfaces e formatos de dados necessários para uma implementação completa foram identificados.

A maior revelação, no entanto, é a lacuna crítica na implementação de referência: a ausência da comunicação real com o kernel. Essa lacuna é intencional no AOSP, pois a responsabilidade de desenvolver essa parte do código recai sobre os fabricantes de dispositivos.

Com base nas nossas descobertas, os próximos passos para a nossa própria implementação são claros e bem definidos:

1. **Replicar as interfaces**: Utilizar o conhecimento adquirido para criar uma implementação da HAL (`ConsumerIrHal.c` ou equivalente) que se alinhe com o padrão AOSP.  
2. **Desenvolver o Módulo de Comunicação**: Focar na lógica interna da nossa HAL para substituir a simulação (`usleep()`) por chamadas reais de `ioctl()` e `write()` para um arquivo de dispositivo que controlaremos.  
3. **Construir o Driver do Kernel**: Desenvolver o driver do kernel que irá ler os comandos do arquivo de dispositivo e orquestrar a comunicação via USB-Serial para o protótipo de hardware.  

Essa abordagem nos permite construir a ponte completa e funcional, do aplicativo de controle remoto até o emissor de infravermelho em nosso hardware customizado.

