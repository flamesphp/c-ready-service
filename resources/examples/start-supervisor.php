<?php

use Flames\Ready\Ready\Service\Worker;
use Flames\Ready\Ready\Service\Supervisor;
use Flames\Ready\Ready\Service\Register;
use Flames\Ready\Ready\Service\Config;

if (php_sapi_name() !== 'cli') {
    http_response_code(403);
    exit('Este script deve ser executado via CLI.');
}

// ini_set funciona aqui pois os INIs são PHP_INI_ALL
// ini_set('flames_ready_service.workers', '8');
// ini_set('flames_ready_service.worker_ttl', '600');

require_once __DIR__ . '/vendor/autoload.php';

class Application
{
    private static \PDO $db;

    public static function boot(): void
    {
        self::$db = new \PDO(
            dsn: 'mysql:host=127.0.0.1;dbname=app;charset=utf8mb4',
            username: 'root',
            password: '',
            options: [\PDO::ATTR_ERRMODE => \PDO::ERRMODE_EXCEPTION]
        );

        fprintf(STDERR,
            "[%s] Worker PID %d pronto (supervisor: %d)\n",
            date('H:i:s'),
            getpid(),
            Supervisor::getPid() ?? 0
        );
    }

    public static function reset(): void {}

    public static function db(): \PDO { return self::$db; }
}

Register::load(Application::class, 'boot');
Register::reset(Application::class, 'reset');

$handler = function (): void {
    $uri = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);

    match (true) {
        $uri === '/'        => handleHome(),
        $uri === '/status'  => handleStatus(),
        $uri === '/scale'   => handleScale(),
        default             => handleNotFound($uri),
    };
};

function handleHome(): void
{
    header('Content-Type: text/html; charset=UTF-8');
    echo '<h1>Worker PID ' . getpid() . '</h1>';
    echo '<p>Requests: ' . Worker::getRequestCount() . '</p>';
}

function handleStatus(): void
{
    header('Content-Type: application/json');
    echo json_encode([
        'supervisor_pid' => Supervisor::getPid(),
        'worker_pids'    => Worker::getPids(),
        'this_worker'    => getpid(),
        'request_count'  => Worker::getRequestCount(),
    ], JSON_PRETTY_PRINT);
}

function handleScale(): void
{
    // Exemplo: escalar para N workers via query string ?n=8
    $n = (int)($_GET['n'] ?? 4);
    Config::setWorkers($n);

    header('Content-Type: application/json');
    echo json_encode(['requested_workers' => $n]);
}

function handleNotFound(string $uri): void
{
    http_response_code(404);
    header('Content-Type: text/plain');
    echo "404 – $uri não encontrado";
}

fprintf(STDERR, "[%s] Iniciando supervisor PID %d...\n", date('H:i:s'), getpid());

Register::request($handler);
