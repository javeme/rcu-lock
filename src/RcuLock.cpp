
class RcuLock
{
private:
	volatile unsigned int m_epoch, m_writing;
	Atomic m_currentReaders, m_nextReaders;
	ConditionVar m_waiter;
	Mutex m_mutex;
public:
	RcuLock() : m_epoch(1), m_writing(0),
		m_currentReaders(0), m_nextReaders(0)
	{
	}

	// for reader
	unsigned int readBegin()
	{
		// consider the synchronization:
		//   1.if nextEpoch() is calling by write thread,
		//     changed share variables are m_writing and m_epoch,
		//     1.1.if writing == 0, m_epoch must be not increased, safe!
		//     1.2.if writing != 0, m_epoch may be increased or not,
		//       1.2.1.if m_epoch increased, thus next epoch, safe!
		//      >1.2.2.if m_epoch not increased(unexpected state, be careful!),
		//         but we can assume m_epoch increased, because m_epoch would
		//         equal to m_writing finally, we just use(return) m_writing
		//         instead of m_epoch, so this is safe!!
		//   2.if moveNextToCurrentEpoch() is calling by write thread,
		//     changed share variables are m_writing and readers,
		//     2.1.if writing != 0, readers must be not moved, safe!
		//     2.2.if writing == 0, readers may be moved OK or not,
		//       2.2.1.if moved OK we'll call m_currentReaders.increase(), safe!
		//      >2.2.2.if not moved from m_nextReaders to m_currentReaders,
		//         (unexpected state, be careful!)
		//         we can't call m_currentReaders.increase(), NOT safe!!!
		//         but we can wait until it's moved OK, so this is safe!!

		unsigned int writing = m_writing;
		smp_rmb();
		unsigned int epoch = this->currentEpoch();

		// it is writing (into next epoch)
		if(writing != 0) {
			m_nextReaders.increase();
			// should return the next epoch if writing
			return writing;
		}
		// no writer now
		else {
			// spin until readers are moved OK if 2.2.2
			while(m_writing == 0 && m_nextReaders > 0);

			m_currentReaders.increase();
			return epoch;
		}
	}

	void readEnd(unsigned int epoch)
	{
		// consider the synchronization:
		//   1.if nextEpoch() is calling by write thread,
		//     changed share variable is only m_writing, safe!
		//   2.if moveNextToCurrentEpoch() is calling by write thread,
		//     changed share variables are m_writing and readers,
		//     2.1.if writing != 0, readers must be not moved, safe!
		//     2.2.if writing == 0, readers may be moved OK or not,
		//       2.2.1.if moved OK we'll call m_currentReaders.increase(), safe!
		//      >2.2.2.if not moved from m_nextReaders to m_currentReaders,
		//         (unexpected state, be careful!)
		//         we can't call m_currentReaders.decreaseAndTest(), NOT safe!!!
		//         but we can wait until it's moved OK, so this is safe!!

		unsigned int writing = m_writing;
		// it is writing (into next epoch) and `epoch` equals to the next epoch
		if(epoch == writing) {
			assert(writing > 0);
			// decrease number of next readers
			// NOTE: m_nextReaders may be < 0 due to m_writing changed above,
			//   like 1.writer set m_writing=0 and clear m_nextReaders;
			//        2.wake up here, now m_nextReaders==0, -1 after decrease();
			m_nextReaders.decrease();

			// NOTE: m_writing may be changed before m_nextReaders.decrease():
			//   non-zero => zero or non-zero => non-zero + 1.
			if(writing != m_writing) {
				// undo (but...how to deal with m_nextReaders=0 after moved OK?)
				m_nextReaders.increase();
				// once again
				return readEnd(epoch);
			}
		}
		// no writer now or `epoch` equals to the current epoch
		else {
			// spin until readers are moved OK if 2.2.2
			while(m_writing == 0 && m_nextReaders > 0);

			// decrease number of current readers and test if it's zero
			// NOTE: this is correct even if m_writing is changed here:
			//   zero => non-zero. because we should also decrease
			//   m_currentReaders after non-writing => writing.
			if(m_currentReaders.decreaseAndTest()) {
				// notify waiter if there is no current readers
				m_waiter.notify();
			}
		}
	}

	// for writer
	void writeBegin()
	{
		m_mutex.lock();
	}

	void writeWait()
	{
		this->waitForReaders();
	}

	void writeEnd()
	{
		m_mutex.unlock();
	}

protected:
	unsigned int currentEpoch()
	{
		return m_epoch;
	}

	unsigned int nextEpoch()
	{
		// we assume m_mutex is locked here
		assert(m_mutex.locked());

		// overflow, jump from 0 to 1 to avoid m_writing=0
		if(m_epoch == -1) {
			m_writing = 1;
			smp_mb(); // to ensure change m_writing before m_epoch for read
			m_epoch = 1;
		}
		// increase m_epoch and set m_writing for the next epoch readers
		else {
			// NOTE: in fact we set m_writing to the next epoch (not current!)
			m_writing = m_epoch + 1;
			smp_mb(); // to ensure change m_writing before m_epoch for read
			++m_epoch;
		}
		return m_epoch;
	}

	void moveNextToCurrentEpoch()
	{
		// we assume m_mutex is locked here
		assert(m_mutex.locked());

		// reset pending next epoch
		m_writing = 0;

		// mb for readBegin()/readEnd()
		// to ensure change m_writing before m_nextReaders/m_currentReaders
		smp_mb();

		while(true) {
			// get the number of next readers
			int next = m_nextReaders;
			// move `next` to m_currentReaders if m_nextReaders not changed
			// between above statement and next statement
			if(m_nextReaders.subAndTest(next)) {
				// NOTE: maybe m_currentReaders != 0 after set m_writing = 0,
				//   so use add(next) instead of set(next)
				m_currentReaders.add(next);
				break;
			}
		}
	}

	void waitForReaders()
	{
		// we assume m_mutex is locked here
		assert(m_mutex.locked());

		int current = this->currentEpoch();
		int readers = m_currentReaders;

		// increase epoch after me
		this->nextEpoch();

		// wait for current readers
		if(readers > 0) {
			//m_waiters[current].wait(m_mutex);
			//m_waiters.remove(current);
			m_waiter.wait(m_mutex);
			assert(m_currentReaders == 0);
		}

		// move the next to the current, and clear the next
		this->moveNextToCurrentEpoch();
	}
};

// test RcuLock
class RcuLockedList
{
private:
	RcuLock m_lock;
	LinkedList<Item> m_list;
public:
	Item read(int index)
	{
		// there is no real read lock here
		unsigned int epoch = m_lock.readBegin();
		Item result = m_list.get(index);
		m_lock.readEnd(epoch);
		return result;
	}

	void write(int index,const Item& value)
	{
		// get write lock
		m_lock.writeBegin();

		// copy and update
		LinkedList<Item>::Node* old = m_list.set(index, value);
		// wait for completion
		m_lock.writeWait();
		// release old item from m_list, like break chain and delete the memory
		m_list.release(old);

		// release write lock
		m_lock.writeEnd();
	}
};
