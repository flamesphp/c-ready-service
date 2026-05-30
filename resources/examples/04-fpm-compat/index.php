<?php
/**
 * 04 – Compatibilidade PHP-FPM / mod_php
 *
 * Flames Ready funciona nos dois modos sem mudar o código da aplicação.
 *
 * MODO WORKER (PHP-CLI):
 *   - boot()  é chamado uma vez quando o worker sobe
 *   - reset() é chamado após cada request pelo loop interno
 *   - xflames_ready_is_ready() retorna true a partir do 2º request
 *
 * MODO PHP-FPM / mod_php (este arquivo):
 *   - xflames_ready_register_load() chama boot() imediatamente
 *     (pois o processo já está executando e as classes já existem)
 *   - xflames_ready_register_reset() agenda reset() para RSHUTDOWN
 *   - xflames_ready_is_ready() retorna true a partir do 2º request
 *     do mesmo process slot do FPM
 *
 * A mesma classe App funciona nos dois cenários sem modificação.
 */

class App
{
    private static bool $booted = false;

    public static function boot(): void
    {
        if (self::$booted) return; // FPM pode chamar várias vezes no mesmo slot

        self::$booted = true;
        // carregar config, conectar DB, instanciar container...
        fprintf(STDERR, "[App] Booted in PID %d\n", getpid());
    }

    public static function reset(): void
    {
        // limpar estado per-request
    }
}

// Em PHP-FPM: boot() é chamado aqui mesmo (processo já está executando).
// Em worker mode: boot() será chamado pelo loop interno após o fork.
if (!xflames_ready_is_ready()) {
    xflames_ready_register_load('App', 'boot');
    xflames_ready_register_reset('App', 'reset');
}

// ── Se estivermos em modo worker, entrar no loop ──────────────────────────
if (defined('FLAMES_READY_WORKER') && FLAMES_READY_WORKER) {
    xflames_ready_handle_request(function (): void {
        handleRequest();
    });
} else {
    // Modo PHP-FPM / mod_php: apenas processar o request atual
    handleRequest();
}

function handleRequest(): void
{
    header('Content-Type: application/json');
    echo json_encode([
        'mode'       => xflames_ready_is_worker() ? 'worker' : 'fpm',
        'pid'        => getpid(),
        'is_ready'   => xflames_ready_is_ready(),
        'req_count'  => xflames_ready_get_request_count(),
    ]);
}
