<?php
/**
 * 02 – Load / Reset callbacks
 *
 * xflames_ready_register_load()  → chamado UMA VEZ por worker, ao iniciar.
 *                                   Bootstrap: conecta DB, instancia container,
 *                                   carrega config, aquece caches, etc.
 *
 * xflames_ready_register_reset() → chamado APÓS cada request.
 *                                   Limpa estado per-request para o próximo
 *                                   request começar limpo.
 *
 * O que NÃO precisa resetar: qualquer coisa que você quer que persista
 * entre requests (conexão DB, DI container, cache em memória, etc).
 */

class App
{
    private static \PDO $db;

    /** Chamado UMA VEZ quando o worker sobe */
    public static function boot(): void
    {
        self::$db = new \PDO('sqlite::memory:');
        self::$db->exec(
            'CREATE TABLE hits (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER)'
        );

        fprintf(STDERR, "[App] Worker PID %d booted – DB ready\n", getpid());
    }

    /** Chamado APÓS cada request */
    public static function reset(): void
    {
        // Em uma app real: resetar request/response objects, fechar sessões,
        // limpar static properties de serviços que acumulam estado, etc.
        // A conexão DB em self::$db permanece viva – isso é intencional.
    }

    public static function db(): \PDO
    {
        return self::$db;
    }
}

// Registrar ANTES de chamar handle_request.
// boot() roda uma vez por worker, NÃO uma vez por request.
xflames_ready_register_load('App', 'boot');
xflames_ready_register_reset('App', 'reset');

xflames_ready_handle_request(function (): void {

    // A conexão DB já existe desde o boot – sem overhead de reconexão
    App::db()->exec('INSERT INTO hits (ts) VALUES (' . time() . ')');

    $total = (int) App::db()->query('SELECT COUNT(*) FROM hits')->fetchColumn();

    header('Content-Type: application/json');
    echo json_encode([
        'worker_pid'      => getpid(),
        'requests_in_db'  => $total,
        'requests_global' => xflames_ready_get_request_count(),
        'uri'             => $_SERVER['REQUEST_URI'] ?? '/',
    ]);
});
