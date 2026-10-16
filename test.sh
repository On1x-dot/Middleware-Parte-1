#!/usr/bin/env bash
BASE="${BASE:-http://localhost:8080}"
fail=0

check() {
    if [ "$2" = "$3" ]; then
        echo "OK     $1"
    else
        echo "FALLO  $1 (esperado: $2, obtenido: $3)"
        fail=1
    fi
}

contains() {
    case "$3" in
        *"$2"*) echo "OK     $1" ;;
        *) echo "FALLO  $1 (esperado que contenga: $2, obtenido: $3)"; fail=1 ;;
    esac
}

status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

header() {
    local name
    name="$(echo "$1" | tr 'A-Z' 'a-z')"
    shift
    curl -s -D - -o /dev/null "$@" | tr -d '\r' | awk -v n="$name" 'tolower($1)==n":" {print $2}'
}

JSON='Content-Type: application/json'

check "middleware /health" 200 "$(status "$BASE/health")"
routes="$(curl -s "$BASE/routes")"
contains "middleware /routes lista la ruta users" '"users":[' "$routes"
contains "middleware /routes lista la ruta products" '"products":[' "$routes"
contains "middleware /routes lista la ruta orders" '"orders":[' "$routes"

check "POST /users crea usuario" 201 "$(status -X POST -H "$JSON" -d '{"name":"Ana","email":"ana@mail.com"}' "$BASE/users")"
w="$(header X-Worker "$BASE/users")"
check "GET /users lo atiende el worker de usuarios" users "${w%%@*}"

check "POST /products crea producto" 201 "$(status -X POST -H "$JSON" -d '{"name":"Teclado","price":49.90,"stock":5}' "$BASE/products")"
w="$(header X-Worker "$BASE/products")"
check "GET /products lo atiende el worker de catalogo" catalog "${w%%@*}"

order="$(curl -s -X POST -H "$JSON" -d '{"userId":1,"productId":1,"qty":2}' "$BASE/orders")"
contains "POST /orders crea pedido con total correcto" '"total":99.80' "$order"
w="$(header X-Worker "$BASE/orders")"
check "GET /orders lo atiende el worker de pedidos" orders "${w%%@*}"

contains "el stock se descuenta en catalogo" '"stock":3' "$(curl -s "$BASE/products/1")"
check "pedido con stock insuficiente responde 409" 409 "$(status -X POST -H "$JSON" -d '{"userId":1,"productId":1,"qty":99}' "$BASE/orders")"
check "pedido con usuario inexistente responde 404" 404 "$(status -X POST -H "$JSON" -d '{"userId":99,"productId":1,"qty":1}' "$BASE/orders")"
check "pedido con producto inexistente responde 404" 404 "$(status -X POST -H "$JSON" -d '{"userId":1,"productId":99,"qty":1}' "$BASE/orders")"
check "usuario invalido responde 400" 400 "$(status -X POST -H "$JSON" -d '{}' "$BASE/users")"
check "ruta sin worker responde 404" 404 "$(status "$BASE/inventario")"
check "X-Request-Id se propaga" abc-123 "$(header X-Request-Id -H 'X-Request-Id: abc-123' "$BASE/users")"

exit $fail
