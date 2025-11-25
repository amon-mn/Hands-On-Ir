// aosp/frameworks/base/services/core/java/com/android/server

/*
 * Copyright (C) 2013 The Android Open Source Project
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

package com.android.server;

import static android.Manifest.permission.TRANSMIT_IR;

import android.annotation.EnforcePermission;
import android.annotation.RequiresNoPermission;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.IConsumerIrService;
import android.hardware.IConsumerIrLearnCallback;
import android.hardware.ir.ConsumerIrFreqRange;
import android.hardware.ir.IConsumerIr;
import android.hardware.ir.IConsumerIrCallback;
import android.hardware.ir.IrEvent;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Slog;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class ConsumerIrService extends IConsumerIrService.Stub {
    private static final String TAG = "ConsumerIrService";

    private static final int MAX_XMIT_TIME = 2000000; /* in microseconds */

    private static native boolean getHidlHalService();
    private static native int halTransmit(int carrierFrequency, int[] rawPattern);
    private static native int[] halGetCarrierFrequencies();

    private final Context mContext;
    private final PowerManager.WakeLock mWakeLock;
    private final boolean mHasNativeHal;
    private final Object mHalLock = new Object();
    private IConsumerIr mAidlService = null;

    // ---------- NOVO: estado de aprendizado (recepção) ----------
    private final Object mLearnLock = new Object();
    private IConsumerIrLearnCallback mLearnCallback = null;
    private final ExecutorService mLearnExecutor =
            Executors.newSingleThreadExecutor();

    // Callback da HAL (AIDL) para o framework
    private final IConsumerIrCallback mHalCallback = new IConsumerIrCallback.Stub() {
        @Override
        public void onIrEvent(IrEvent event) {
            handleIrEvent(event);
        }
    };
    // ------------------------------------------------------------

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
            Slog.i(TAG, "Using AIDL ConsumerIr HAL");
            return true;
        }

        // Fall back to the HIDL HAL service
        boolean hasHidl = getHidlHalService();
        if (hasHidl) {
            Slog.i(TAG, "Using HIDL ConsumerIr HAL");
        }
        return hasHidl;
    }

    private void throwIfNoIrEmitter() {
        if (!mHasNativeHal) {
            throw new UnsupportedOperationException("IR emitter not available");
        }
    }

    // ============================================================
    // TRANSMISSÃO (TX) - CÓDIGO ORIGINAL
    // ============================================================

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

    // ============================================================
    // NOVO: RECEPÇÃO / APRENDIZADO (RX)
    // ============================================================

    @Override
    @EnforcePermission(TRANSMIT_IR)
    public void startLearning(String packageName, IConsumerIrLearnCallback callback) {
        super.startLearning_enforcePermission();

        throwIfNoIrEmitter();

        synchronized (mLearnLock) {
            if (mLearnCallback != null) {
                Slog.w(TAG, "startLearning: already in progress");
                return;
            }
            mLearnCallback = callback;
        }

        synchronized (mHalLock) {
            if (mAidlService == null) {
                Slog.e(TAG, "startLearning: AIDL HAL not available (HIDL não suporta RX)");
                synchronized (mLearnLock) {
                    mLearnCallback = null;
                }
                return;
            }

            try {
                Slog.i(TAG, "Calling HAL startReceive()");
                mAidlService.startReceive(mHalCallback);
            } catch (RemoteException e) {
                Slog.e(TAG, "Error starting learning on HAL", e);
                synchronized (mLearnLock) {
                    mLearnCallback = null;
                }
            }
        }
    }

    @Override
    @EnforcePermission(TRANSMIT_IR)
    public void stopLearning(String packageName) {
        super.stopLearning_enforcePermission();

        throwIfNoIrEmitter();

        synchronized (mLearnLock) {
            mLearnCallback = null;
        }

        synchronized (mHalLock) {
            if (mAidlService == null) {
                // HIDL não tem suporte a RX, então nada a fazer
                return;
            }
            try {
                Slog.i(TAG, "Calling HAL stopReceive()");
                mAidlService.stopReceive();
            } catch (RemoteException e) {
                Slog.e(TAG, "Error stopping learning on HAL", e);
            }
        }
    }

    /**
     * Chamado pelo callback da HAL (mHalCallback) quando um IrEvent é recebido.
     * Aqui traduzimos o evento da HAL para o callback de framework/app.
     */
    private void handleIrEvent(IrEvent event) {
        final IConsumerIrLearnCallback callback;

        synchronized (mLearnLock) {
            if (mLearnCallback == null) {
                Slog.w(TAG, "handleIrEvent: no learn callback registered; dropping event");
                return;
            }
            callback = mLearnCallback;
        }

        mLearnExecutor.execute(() -> {
            try {
                callback.onLearned(
                        event.carrierFrequencyHz,
                        event.rawPattern,
                        event.timestampNanos / 1000000L
                );
            } catch (RemoteException e) {
                Slog.e(TAG, "App learn callback failed", e);
            }
        });
    }
}
