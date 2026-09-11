/*++

Module Name:

    cable.cpp

Abstract:

    Реализация кольцевого буфера виртуального кабеля. См. cable.h.

--*/

#include "definitions.h"
#include "cable.h"

#define CABLE_POOLTAG 'lbCB'

CCableBuffer g_Cable;

#pragma code_seg("PAGE")
NTSTATUS CCableBuffer::Init(_In_ ULONG capacityBytes, _In_ ULONG primeBytes)
{
    PAGED_CODE();

    capacityBytes -= capacityBytes % CABLE_BLOCK_ALIGN;
    KeInitializeSpinLock(&m_lock);
    m_buffer = (BYTE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, capacityBytes, CABLE_POOLTAG);
    if (m_buffer == NULL)
    {
        m_capacity = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_capacity = capacityBytes;
    m_prime = primeBytes < capacityBytes / 2 ? primeBytes : capacityBytes / 2;
    m_head = 0;
    m_count = 0;
    m_headPhase = 0;
    m_primed = FALSE;
    m_underruns = 0;
    m_overruns = 0;
    m_realigns = 0;
    m_trimmedBytes = 0;
    m_lastReport = 0;
    RtlZeroMemory(m_reported, sizeof(m_reported));
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
void CCableBuffer::Free()
{
    PAGED_CODE();

    if (m_buffer != NULL)
    {
        ExFreePoolWithTag(m_buffer, CABLE_POOLTAG);
        m_buffer = NULL;
    }
    m_capacity = 0;
    m_count = 0;
}

#pragma code_seg()
void CCableBuffer::Reset()
{
    if (m_buffer == NULL)
    {
        return;
    }
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    m_head = 0;
    m_count = 0;
    m_headPhase = 0;
    m_primed = FALSE;
    KeReleaseSpinLock(&m_lock, oldIrql);
}

#pragma code_seg()
void CCableBuffer::Write(_In_reads_bytes_(bytes) const BYTE* data, _In_ ULONG bytes, _In_ ULONGLONG streamPos)
{
    if (m_buffer == NULL || bytes == 0)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    // Фаза кадра в голове должна совпадать с фазой render-потока. Расхождение
    // возможно после Reset/переполнения при ненулевой позиции: подгоняем голову
    // нулями (портит не больше одного кадра).
    const ULONG phase = (ULONG)(streamPos % CABLE_BLOCK_ALIGN);
    if (phase != m_headPhase)
    {
        ULONG pad = (phase + CABLE_BLOCK_ALIGN - m_headPhase) % CABLE_BLOCK_ALIGN;
        for (ULONG i = 0; i < pad; ++i)
        {
            m_buffer[(m_head + i) % m_capacity] = 0;
        }
        m_head = (m_head + pad) % m_capacity;
        m_count = (m_count + pad > m_capacity) ? m_capacity : m_count + pad;
        m_headPhase = phase;
        m_realigns++;
    }

    if (bytes > m_capacity)
    {
        data += bytes - m_capacity;
        bytes = m_capacity;
    }

    ULONG first = m_capacity - m_head;
    if (first > bytes)
    {
        first = bytes;
    }
    RtlCopyMemory(m_buffer + m_head, data, first);
    if (bytes > first)
    {
        RtlCopyMemory(m_buffer, data + first, bytes - first);
    }
    m_head = (m_head + bytes) % m_capacity;
    m_headPhase = (m_headPhase + bytes) % CABLE_BLOCK_ALIGN;

    if (m_count + bytes > m_capacity)
    {
        // Переполнение: самые старые данные затёрты, хвост потерял фазу.
        m_overruns++;
        m_count = m_capacity;
        m_primed = FALSE;
    }
    else
    {
        m_count += bytes;
    }

    KeReleaseSpinLock(&m_lock, oldIrql);
    ReportCounters();
}

#pragma code_seg()
void CCableBuffer::Read(_Out_writes_bytes_all_(bytes) BYTE* data, _In_ ULONG bytes, _In_ ULONGLONG streamPos)
{
    if (bytes == 0)
    {
        return;
    }
    if (m_buffer == NULL)
    {
        RtlZeroMemory(data, bytes);
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    if (!m_primed)
    {
        if (m_count >= m_prime + CABLE_BLOCK_ALIGN)
        {
            // Пока никто не читал, render-сторона могла накопить до capacity
            // (вплоть до секунды). Задержка кабеля = m_prime, поэтому всё, что
            // старше, отбрасываем: иначе backlog остаётся в задержке навсегда,
            // так как обе стороны идут от одного QPC и не сближаются.
            if (m_count > m_prime + CABLE_BLOCK_ALIGN)
            {
                m_trimmedBytes += m_count - (m_prime + CABLE_BLOCK_ALIGN);
                m_count = m_prime + CABLE_BLOCK_ALIGN;
            }
            // Выравниваем хвост под фазу кадра capture-потока: байт, который
            // ляжет в позицию streamPos, должен иметь ту же фазу в render-потоке.
            const ULONG wantPhase = (ULONG)(streamPos % CABLE_BLOCK_ALIGN);
            const ULONG tailPhase = (m_headPhase + m_capacity * 4 - (m_count % CABLE_BLOCK_ALIGN)) % CABLE_BLOCK_ALIGN;
            const ULONG skip = (wantPhase + CABLE_BLOCK_ALIGN - tailPhase) % CABLE_BLOCK_ALIGN;
            m_count -= skip;
            m_primed = TRUE;
        }
        else
        {
            KeReleaseSpinLock(&m_lock, oldIrql);
            RtlZeroMemory(data, bytes);
            return;
        }
    }

    ULONG n = bytes < m_count ? bytes : m_count;
    ULONG tail = (m_head + m_capacity - m_count) % m_capacity;
    ULONG first = m_capacity - tail;
    if (first > n)
    {
        first = n;
    }
    RtlCopyMemory(data, m_buffer + tail, first);
    if (n > first)
    {
        RtlCopyMemory(data + first, m_buffer, n - first);
    }
    m_count -= n;

    if (n < bytes)
    {
        // Опустошение: тишина до конца запроса, дальше снова ждём накопления.
        RtlZeroMemory(data + n, bytes - n);
        m_primed = FALSE;
        m_underruns++;
    }

    KeReleaseSpinLock(&m_lock, oldIrql);
    ReportCounters();
}

#pragma code_seg()
void CCableBuffer::ReportCounters()
{
    // Не чаще раза в секунду (единицы 100 ns) и только при изменении. Read и Write
    // идут из DPC разных потоков: слот отчёта захватывается атомарно. Счётчики
    // читаются без m_lock: 64-битное выровненное чтение на x64 атомарно, отчёту
    // хватает значения на момент чтения.
    const LONG64 now = (LONG64)KeQueryInterruptTime();
    const LONG64 last = m_lastReport;
    if (now - last < 10000000)
    {
        return;
    }
    if (InterlockedCompareExchange64(&m_lastReport, now, last) != last)
    {
        return;
    }
    const ULONGLONG current[4] = { m_underruns, m_overruns, m_realigns, m_trimmedBytes };
    if (RtlCompareMemory(current, m_reported, sizeof(current)) == sizeof(current))
    {
        return;
    }
    RtlCopyMemory(m_reported, current, sizeof(current));
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_WARNING_LEVEL,
               "bomboec_cable: underruns %I64u, overruns %I64u, realigns %I64u, trimmed %I64u bytes\n",
               current[0], current[1], current[2], current[3]);
}
