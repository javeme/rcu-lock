
class RcuLock
{
private:
	volatile unsigned int m_epoch, m_writing;
	Atomic m_currentReaders, m_nextReaders;
	ConditionVar m_waiter;
	Mutex m_mutex; // for writers
	SpinRWLock m_rwLock; // for writer and readers
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
		//         so we should add lock for this.
		//   2.if moveNextToCurrentEpoch() is calling by write thread,
		//     changed share variables are m_writing and readers,
		//     2.1.if writing != 0, readers must be not moved, safe!
		//     2.2.if writing == 0, readers may be moved OK or not,
		//       2.2.1.if moved OK we'll call m_currentReaders.increase(), safe!
		//      >2.2.2.if not moved from m_nextReaders to m_currentReaders,
		//         (unexpected state, be careful!)
		//         we can't call m_currentReaders.increase(), NOT safe!!!
		//         so we should add lock for this.

		m_rwLock.lockRead();

		unsigned int epoch = this->currentEpoch();

		// it is writing (into next epoch)
		if(m_writing != 0) {
			m_nextReaders.increase();
			// should return the next epoch if writing
			epoch = m_writing;
		}
		// no writer now
		else {
			m_currentReaders.increase();
		}

		m_rwLock.unlockRead();

		return epoch;
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
		//         so we should add lock for this.

		m_rwLock.lockRead();

		unsigned int writing = m_writing;

		// it is writing (into next epoch) and `epoch` equals to the next epoch
		if(epoch == m_writing) {
			assert(writing > 0);
			// decrease number of next readers
			m_nextReaders.decrease();
		}
		// no writer now or `epoch` equals to the current epoch
		else {
			// decrease number of current readers and test if it's zero
			if(m_currentReaders.decrease() == 0) {
				// notify waiter if there is no current readers
				if(writing > 0)
					m_waiter.notify();
			}
		}

		m_rwLock.unlockRead();
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

	int nextEpoch()
	{
		// we assume m_mutex is locked here
		assert(m_mutex.locked());

		m_rwLock.lockWrite();

		// overflow, jump from 0 to 1 to avoid m_writing=0
		if(m_epoch == -1) {
			m_writing = 1;
		}
		// set m_writing for the next epoch readers
		else {
			// NOTE: in fact we set m_writing to next epoch (not current epoch!)
			m_writing = m_epoch + 1;
		}

		int readers = m_currentReaders.load();

		m_rwLock.unlockWrite();

		return readers;
	}

	void moveNextToCurrentEpoch()
	{
		// we assume m_mutex is locked here
		assert(m_mutex.locked());

		m_rwLock.lockWrite();

		// set next epoch to m_epoch and reset m_writing
		m_epoch = m_writing;
		m_writing = 0;

		// get the number of next readers
		int next = m_nextReaders;
		// move `next` to m_currentReaders
		m_nextReaders.sub(next);
		m_currentReaders.add(next);
		assert(m_nextReaders == 0);

		m_rwLock.unlockWrite();
	}

	void waitForReaders()
	{
		// we assume m_mutex is locked here
		assert(m_mutex.locked());

		// increase epoch after me
		int readers = this->nextEpoch();

		// wait for current readers
		if(readers > 0) {
			//m_waiters[current].wait(m_mutex);
			//m_waiters.remove(current);
			// NOTE: m_currentReaders may be decreased before wait()
			m_waiter.wait();
			assert(m_currentReaders == 0);
		}

		// move the next to the current, and clear the next
		this->moveNextToCurrentEpoch();
	}
};

// test RcuLock
template <class Item>
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
