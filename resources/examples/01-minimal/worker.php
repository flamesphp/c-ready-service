<?php

use Flames\Ready\Ready\Service\Worker;
use Flames\Ready\Ready\Service\Register;

Register::request(function (): void {
    header('Content-Type: text/plain');
    echo "Hello from worker PID " . getpid() . "\n";
    echo "Requests: " . Worker::getRequestCount() . "\n";
    echo "URI: " . ($_SERVER['REQUEST_URI'] ?? '/') . "\n";
});
