package android.hardware;

/**
 * Callback chamado pelo ConsumerIrService quando um comando
 * IR for aprendido ou ocorrer um erro durante o aprendizado.
 *
 * {@hide}
 */
interface IConsumerIrLearnCallback {

    /**
     * Chamado quando um comando IR foi capturado com sucesso.
     *
     * @param carrierFrequencyHz Frequência da portadora em Hz.
     * @param pattern Padrão ON/OFF em microssegundos.
     * @param timestampMillis Momento da captura (System.currentTimeMillis()).
     */
    void onLearned(int carrierFrequencyHz, in int[] pattern, long timestampMillis);

    /**
     * Chamado em caso de erro.
     *
     * @param errorCode Código de erro definido pelo serviço.
     * @param message Mensagem descritiva.
     */
    void onError(int errorCode, String message);
}
