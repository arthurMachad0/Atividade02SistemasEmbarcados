#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Inclusão de bibliotecas do FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Inclusão de bibliotecas do ADC e da ESP-IDF
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

// ============================================================
// DEFINIÇÕES
// ============================================================

// Pino onde o LED está conectado
#define LED_PIN 14

// Resolução do timer PWM
// 12 bits -> valores de 0 até 4095
#define PWM_DUTY_RESOLUTION LEDC_TIMER_12_BIT
#define PWM_DUTY_MAX 4095

// Tensão máxima esperada no ADC
#define TENSAO_MAXIMA 3300

// Quantidade de leituras utilizadas para calcular a média
#define NUMERO_LEITURAS 10

// ============================================================
// TAGS PARA OS LOGS
// ============================================================

const static char *TAG = "ADC Test";
const static char *TAG_LED = "Tensao Led";

// ============================================================
// VARIÁVEIS GLOBAIS
// ============================================================

// Valor bruto lido pelo ADC
static int leitura_adc;

// Tensão convertida para mV
static int tensao;

// ============================================================
// DECLARAÇÃO DAS FUNÇÕES DE CALIBRAÇÃO
// ============================================================

static bool iniciar_calibracao_adc(
    adc_unit_t unidade,
    adc_channel_t canal,
    adc_atten_t atenuacao,
    adc_cali_handle_t *ponteiro_controlador);

static void encerrar_calibracao_adc(
    adc_cali_handle_t controlador);

// ============================================================
// CONFIGURAÇÃO DO PWM
// ============================================================

void configurar_pwm()
{
    // Configuração do timer PWM
    ledc_timer_config_t config_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .duty_resolution = PWM_DUTY_RESOLUTION,
        .clk_cfg = LEDC_AUTO_CLK};

    ledc_timer_config(&config_timer);

    // Configuração do canal PWM
    ledc_channel_config_t config_canal_pwm = {
        .channel = LEDC_CHANNEL_1,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_PIN,
        .duty = 0};

    ledc_channel_config(&config_canal_pwm);
}

// ============================================================
// PROGRAMA PRINCIPAL
// ============================================================

void app_main(void)
{
    // --------------------------------------------------------
    // Inicialização do PWM
    // --------------------------------------------------------

    configurar_pwm();

    // --------------------------------------------------------
    // Configuração do ADC
    // --------------------------------------------------------

    adc_oneshot_unit_handle_t controlador_adc;

    // Configuração da unidade ADC1
    adc_oneshot_unit_init_cfg_t configuracao_adc = {
        .unit_id = ADC_UNIT_1};

    // Inicializa o ADC1
    ESP_ERROR_CHECK(
        adc_oneshot_new_unit(
            &configuracao_adc,
            &controlador_adc));

    // --------------------------------------------------------
    // Configuração do canal ADC
    // --------------------------------------------------------

    adc_oneshot_chan_cfg_t configuracao_canal_adc = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12};

    // Configura o canal 0 do ADC1
    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            controlador_adc,
            ADC_CHANNEL_0,
            &configuracao_canal_adc));

    // --------------------------------------------------------
    // Inicialização da calibração
    // --------------------------------------------------------

    adc_cali_handle_t controlador_calibracao = NULL;

    bool calibracao_ativa = iniciar_calibracao_adc(
        ADC_UNIT_1,
        ADC_CHANNEL_0,
        ADC_ATTEN_DB_12,
        &controlador_calibracao);

    // ========================================================
    // LOOP PRINCIPAL
    // ========================================================

    while (1)
    {
        // ----------------------------------------------------
        // Variáveis para calcular a média das leituras
        // ----------------------------------------------------

        int soma_leituras = 0;
        int media_leituras;

        // ----------------------------------------------------
        // Realiza várias leituras do potenciômetro
        // ----------------------------------------------------

        for (int i = 0; i < NUMERO_LEITURAS; i++)
        {
            ESP_ERROR_CHECK(
                adc_oneshot_read(
                    controlador_adc,
                    ADC_CHANNEL_0,
                    &leitura_adc));

            soma_leituras += leitura_adc;

            // Pequeno intervalo entre as leituras
            vTaskDelay(2 / portTICK_PERIOD_MS);
        }

        // ----------------------------------------------------
        // Calcula a média das leituras
        // ----------------------------------------------------

        media_leituras = soma_leituras / NUMERO_LEITURAS;

        // Mostra o valor médio bruto no terminal
        ESP_LOGI(
            TAG,
            "ADC%d Channel[%d] Raw Data: %d",
            ADC_UNIT_1 + 1,
            ADC_CHANNEL_0,
            media_leituras);

        // ----------------------------------------------------
        // Conversão do valor ADC para tensão
        // ----------------------------------------------------

        if (calibracao_ativa)
        {
            ESP_ERROR_CHECK(
                adc_cali_raw_to_voltage(
                    controlador_calibracao,
                    media_leituras,
                    &tensao));

            // Mostra a tensão no terminal
            ESP_LOGI(
                TAG,
                "Tensao medida: %d mV",
                tensao);

            // =================================================
            // CONVERSÃO DA TENSÃO PARA O PWM
            // =================================================
            //
            // 0 mV    -> PWM 0    -> LED apagado
            // 1650 mV -> PWM 2047 -> aproximadamente 50%
            // 3300 mV -> PWM 4095 -> 100%
            //
            // =================================================

            uint32_t ciclo_trabalho;

            // Caso a tensão seja menor ou igual a zero
            if (tensao <= 0)
            {
                ciclo_trabalho = 0;
            }

            // Caso a tensão seja igual ou superior a 3,3 V
            else if (tensao >= TENSAO_MAXIMA)
            {
                ciclo_trabalho = PWM_DUTY_MAX;
            }

            // Para valores intermediários
            else
            {
                ciclo_trabalho =
                    ((uint32_t)tensao * PWM_DUTY_MAX) / TENSAO_MAXIMA;
            }

            // ------------------------------------------------
            // Atualiza o PWM do LED
            // ------------------------------------------------

            ledc_set_duty(
                LEDC_LOW_SPEED_MODE,
                LEDC_CHANNEL_1,
                ciclo_trabalho);

            ledc_update_duty(
                LEDC_LOW_SPEED_MODE,
                LEDC_CHANNEL_1);

            // ------------------------------------------------
            // Mostra informações no terminal
            // ------------------------------------------------

            ESP_LOGI(
                TAG_LED,
                "Tensao: %d mV | PWM: %lu / %d",
                tensao,
                ciclo_trabalho,
                PWM_DUTY_MAX);
        }

        // ----------------------------------------------------
        // Atualiza aproximadamente 50 vezes por segundo
        // ----------------------------------------------------

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    // ========================================================
    // ENCERRAMENTO
    // ========================================================
    // Este trecho nunca será atingido porque o while(1)
    // é infinito.
    // ========================================================

    ESP_ERROR_CHECK(
        adc_oneshot_del_unit(controlador_adc));

    if (calibracao_ativa)
    {
        encerrar_calibracao_adc(
            controlador_calibracao);
    }
}

// ============================================================
// INICIALIZAÇÃO DA CALIBRAÇÃO DO ADC
// ============================================================

static bool iniciar_calibracao_adc(
    adc_unit_t unidade,
    adc_channel_t canal,
    adc_atten_t atenuacao,
    adc_cali_handle_t *ponteiro_controlador)
{
    adc_cali_handle_t controlador_local = NULL;

    esp_err_t resultado = ESP_FAIL;

    bool esta_calibrado = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED

    if (!esta_calibrado)
    {
        ESP_LOGI(
            TAG,
            "calibration scheme version is %s",
            "Curve Fitting");

        // Configuração da curva de calibração
        adc_cali_curve_fitting_config_t config_calibracao = {
            .unit_id = unidade,
            .chan = canal,
            .atten = atenuacao,
            .bitwidth = ADC_BITWIDTH_12};

        // Inicializa calibração por curva
        resultado = adc_cali_create_scheme_curve_fitting(
            &config_calibracao,
            &controlador_local);

        if (resultado == ESP_OK)
        {
            esta_calibrado = true;
        }
    }

#endif

    // --------------------------------------------------------
    // Calibração utilizando Line Fitting
    // --------------------------------------------------------

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED

    if (!esta_calibrado)
    {
        ESP_LOGI(
            TAG,
            "calibration scheme version is %s",
            "Line Fitting");

        // Configuração da linha de calibração
        adc_cali_line_fitting_config_t config_calibracao = {
            .unit_id = unidade,
            .atten = atenuacao,
            .bitwidth = ADC_BITWIDTH_12};

        // Inicializa calibração por linha
        resultado = adc_cali_create_scheme_line_fitting(
            &config_calibracao,
            &controlador_local);

        if (resultado == ESP_OK)
        {
            esta_calibrado = true;
        }
    }

#endif

    // --------------------------------------------------------
    // Salva o controlador de calibração
    // --------------------------------------------------------

    *ponteiro_controlador = controlador_local;

    // --------------------------------------------------------
    // Mensagens de diagnóstico
    // --------------------------------------------------------

    if (resultado == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Calibration Success");
    }
    else if (
        resultado == ESP_ERR_NOT_SUPPORTED ||
        !esta_calibrado)
    {
        ESP_LOGW(
            TAG,
            "eFuse not burnt, skip software calibration");
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Invalid arg or no memory");
    }

    return esta_calibrado;
}

// ============================================================
// FINALIZAÇÃO DA CALIBRAÇÃO
// ============================================================

static void encerrar_calibracao_adc(
    adc_cali_handle_t controlador)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED

    ESP_LOGI(
        TAG,
        "deregister %s calibration scheme",
        "Curve Fitting");

    ESP_ERROR_CHECK(
        adc_cali_delete_scheme_curve_fitting(controlador));

#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED

    ESP_LOGI(
        TAG,
        "deregister %s calibration scheme",
        "Line Fitting");

    ESP_ERROR_CHECK(
        adc_cali_delete_scheme_line_fitting(controlador));

#endif
}