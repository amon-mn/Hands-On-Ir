package android.hardware.ir;

@VintfStability
parcelable ConsumerIrCapture {
    /**
     * Frequência portadora estimada, em Hertz.
     */
    int frequencyHz;

    /**
     * Padrão de pulsos da captura, em microssegundos.
     *
     * Os valores são alternados "on/off" (marca/espaço), como no transmit():
     * [on1, off1, on2, off2, ...].
     */
    int[] patternMicros;
}
