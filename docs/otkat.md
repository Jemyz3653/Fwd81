# Ручной откат Fwd81 из системы

Этот файл — страховка на случай, если проверка внедрения через IFEO пойдёт не так
и `fwd81cli uninstall` по какой-то причине не сработает. Здесь точные команды,
которыми **Арсений сам**, из командной строки, запущенной **от имени
администратора**, уберёт всё, что Fwd81 мог положить в систему.

Fwd81 добавляет в систему ровно две вещи:
1. файл `fwd81core.dll` в `C:\Windows\System32`;
2. значения `GlobalFlag` и `VerifierDlls` в ветке IFEO для конкретной программы.

Больше ничего. Чужие файлы Microsoft не трогаются.

---

## Быстрый откат для пробника M2 (fwd81probe.exe)

Открыть **cmd от имени администратора** и выполнить:

```
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\fwd81probe.exe" /f
del "%SystemRoot%\System32\fwd81core.dll"
```

Первая команда убирает ключ IFEO пробника целиком. Вторая — наш файл из System32.
Если какой-то команды «нечего удалять» (ключа или файла нет) — это нормально,
значит эта часть уже чиста.

---

## Проверить, что всё вычищено

```
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\fwd81probe.exe"
dir "%SystemRoot%\System32\fwd81core.dll"
```

Ожидается на обе команды: «не удаётся найти» / «Файл не найден». Это значит,
следов Fwd81 в системе не осталось.

---

## Если Fwd81 был включён для другой программы

Подставь её имя вместо `fwd81probe.exe`:

```
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ИМЯ.exe" /f
```

Посмотреть, для каких программ вообще заведены ключи IFEO (чтобы найти наши):

```
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options"
```

Наши ключи узнаются по значению `VerifierDlls` = `fwd81core.dll` внутри:

```
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ИМЯ.exe" /v VerifierDlls
```

**Важно:** удаляй ключ программы только если он был заведён Fwd81. Если у программы
там есть другие значения (например, `Debugger` от чужого софта), не сноси ключ
целиком — удали только наши значения:

```
reg delete "HKLM\...\Image File Execution Options\ИМЯ.exe" /v VerifierDlls /f
reg delete "HKLM\...\Image File Execution Options\ИМЯ.exe" /v GlobalFlag /f
```

---

## Крайний случай

Если система не загружается из-за ошибочно включённого критического процесса
(команда `enable` такое запрещает, но на всякий случай): загрузиться в
безопасном режиме или со среды восстановления и выполнить те же `reg delete` и
`del` из-под неё, либо откатиться на точку восстановления, созданную перед
проверкой.
