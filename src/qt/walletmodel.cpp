#include "walletmodel.h"
#include "guiconstants.h"
#include "optionsmodel.h"
#include "addresstablemodel.h"
#include "transactiontablemodel.h"

#include "ui_interface.h"
#include "wallet.h"
#include "walletdb.h" // for BackupWallet

#include <QSet>

WalletModel::WalletModel(CWallet *wallet, OptionsModel *optionsModel, QObject *parent) :
QObject(parent), wallet(wallet), optionsModel(optionsModel), addressTableModel(0),
transactionTableModel(0),
cachedBalance(0), cachedUnconfirmedBalance(0), cachedNumTransactions(0),
cachedEncryptionStatus(Unencrypted), cachedMintStatus(0), cachedReserveBalance(0)
{
	addressTableModel = new AddressTableModel(wallet, this);
	transactionTableModel = new TransactionTableModel(wallet, this);
}

qint64 WalletModel::getBalance() const
{
	return wallet->GetBalance();
}

qint64 WalletModel::getStake() const
{
	return wallet->GetStake();
}

qint64 WalletModel::getUnconfirmedBalance() const
{
	return wallet->GetUnconfirmedBalance();
}

int WalletModel::getNumTransactions() const
{
	int numTransactions = 0;
	{
		LOCK(wallet->cs_wallet);
		numTransactions = wallet->mapWallet.size();
	}
	return numTransactions;
}

void WalletModel::update()
{
	qint64 newBalance = getBalance();
	qint64 newUnconfirmedBalance = getUnconfirmedBalance();
	int newNumTransactions = getNumTransactions();
	EncryptionStatus newEncryptionStatus = getEncryptionStatus();
	bool newMintStatus = getMintUnlockedbool();
        qint64 newReserveBalance = getCoinStakeReserveValue();

	if (cachedBalance != newBalance || cachedUnconfirmedBalance != newUnconfirmedBalance)
		emit balanceChanged(newBalance, getStake(), newUnconfirmedBalance);

	if (cachedNumTransactions != newNumTransactions)
		emit numTransactionsChanged(newNumTransactions);

	if (cachedEncryptionStatus != newEncryptionStatus)
		emit encryptionStatusChanged(newEncryptionStatus);
        
	if (cachedMintStatus != newMintStatus)
		emit mintStatusChanged(newMintStatus,newReserveBalance);

	cachedBalance = newBalance;
	cachedUnconfirmedBalance = newUnconfirmedBalance;
	cachedNumTransactions = newNumTransactions;
	cachedMintStatus = newMintStatus;
        cachedReserveBalance = newReserveBalance;
}

void WalletModel::updateAddressList()
{
	addressTableModel->update();
}

bool WalletModel::validateAddress(const QString &address)
{
	CBitcoinAddress addressParsed(address.toStdString());
	return addressParsed.IsValid();
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(const QList<SendCoinsRecipient> &recipients)
{
	qint64 total = 0;
	QSet<QString> setAddress;
	QString hex;

	if (recipients.empty())
	{
		return OK;
	}

	// Pre-check input data for validity
	foreach(const SendCoinsRecipient &rcp, recipients)
	{
		if (!validateAddress(rcp.address))
		{
			return InvalidAddress;
		}
		setAddress.insert(rcp.address);

		if (rcp.amount < MIN_TXOUT_AMOUNT)
		{
			return InvalidAmount;
		}
		total += rcp.amount;
	}

	if (recipients.size() > setAddress.size())
	{
		return DuplicateAddress;
	}

	if (total > getBalance())
	{
		return AmountExceedsBalance;
	}

	if ((total + nTransactionFee) > getBalance())
	{
		return SendCoinsReturn(AmountWithFeeExceedsBalance, nTransactionFee);
	}

	{
		LOCK2(cs_main, wallet->cs_wallet);

		// Sendmany
		std::vector<std::pair<CScript, int64> > vecSend;
		foreach(const SendCoinsRecipient &rcp, recipients)
		{
			CScript scriptPubKey;
			scriptPubKey.SetBitcoinAddress(rcp.address.toStdString());
			vecSend.push_back(make_pair(scriptPubKey, rcp.amount));
		}

		CWalletTx wtx;
		CReserveKey keyChange(wallet);
		int64 nFeeRequired = 0;
		bool fCreated = wallet->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired);

		if (!fCreated)
		{
			if ((total + nFeeRequired) > wallet->GetBalance())
			{
				return SendCoinsReturn(AmountWithFeeExceedsBalance, nFeeRequired);
			}
			return TransactionCreationFailed;
		}
		if (!ThreadSafeAskFee(nFeeRequired, tr("Sending...").toStdString()))
		{
			return Aborted;
		}
		if (!wallet->CommitTransaction(wtx, keyChange))
		{
			return TransactionCommitFailed;
		}
		hex = QString::fromStdString(wtx.GetHash().GetHex());
	}
	// Add addresses / update labels that we've sent to to the address book
	foreach(const SendCoinsRecipient &rcp, recipients)
	{
		std::string strAddress = rcp.address.toStdString();
		std::string strLabel = rcp.label.toStdString();
		{
			LOCK(wallet->cs_wallet);

			std::map<CBitcoinAddress, std::string>::iterator mi = wallet->mapAddressBook.find(strAddress);

			// Check if we have a new address or an updated label
			if (mi == wallet->mapAddressBook.end() || mi->second != strLabel)
			{
				wallet->SetAddressBookName(strAddress, strLabel);
			}
		}
	}
	if (wallet->Lock())//lock wallet once send completes
	{
		wallet->setfWalletUnlockMintOnlyState(false); //make sure to set fWalletUnlockMintOnly bool in wallet is set to false because wallet is locked 
	}
	return SendCoinsReturn(OK, 0, hex);
}

OptionsModel *WalletModel::getOptionsModel()
{
	return optionsModel;
}

AddressTableModel *WalletModel::getAddressTableModel()
{
	return addressTableModel;
}

TransactionTableModel *WalletModel::getTransactionTableModel()
{
	return transactionTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
	if (!wallet->IsCrypted())
	{
		return Unencrypted;
	}
	else if (wallet->IsLocked())
	{
		return Locked;
	}
	else
	{
		return Unlocked;
	}
}

bool WalletModel::setWalletEncrypted(bool encrypted, const SecureString &passphrase)
{
	if (encrypted)
	{
		// Encrypt
		return wallet->EncryptWallet(passphrase);
	}
	else
	{
		// Decrypt -- TODO; not supported yet
		return false;
	}
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase)
{
	if (locked)
	{
		// Lock
		wallet->setfWalletUnlockMintOnlyState(false); //make sure to set fWalletUnlockMintOnly bool in wallet is set to false because wallet is locked 
		return wallet->Lock();
	}
	else
	{
		// Unlock
		return wallet->Unlock(passPhrase);
	}
}

void WalletModel::setCoinStakeReserveValue(qint64 reserveval)
{
	wallet->setCoinStakeReserve(reserveval);
	return;
}

qint64 WalletModel::getCoinStakeReserveValue()
{
	return wallet->getCoinStakeReserve();
}

void WalletModel::setMintUnlockedbool(bool setmintstatus)
{//set bool fWalletUnlockMintOnly in wallet to true to prevent hack coin sends
	//must be called after unlocking wallet to allow for minting
	wallet->setfWalletUnlockMintOnlyState(setmintstatus);

	return;
}

bool WalletModel::getMintUnlockedbool()
{//return bool fWalletUnlockMintOnly; see setMintUnlockedbool 

	return wallet->getfWalletUnlockMintOnlyState();
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
	bool retval;
	{   
	LOCK(wallet->cs_wallet);
	wallet->Lock(); // Make sure wallet is locked before attempting pass change
	setMintUnlockedbool(false);//set minting bool to false just in case because locked
	retval = wallet->ChangeWalletPassphrase(oldPass, newPass);
	}
	return retval;
}

bool WalletModel::backupWallet(const QString &filename)
{
	return BackupWallet(*wallet, filename.toLocal8Bit().data());
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
	bool was_locked = getEncryptionStatus() == Locked;
	if ((!was_locked) && fWalletUnlockMintOnly)
	{//curently minting, cannot continue
		//warn user that minting on, and tell user to manually stop minting 
		emit warnMinting(); //warn of minting
		return UnlockContext(this, false, false); //return invalid context to stop further processing
		/**********************************************/
		/*alternate implimentation - just stop minting with no warning*/
		//wallet->setfWalletUnlockMintOnlyState(false);//stop minting bool
		//setWalletLocked(true);//lock wallet
		//was_locked = getEncryptionStatus() == Locked;
		/**********************************************/


	}
	if (was_locked)
	{
		// Request UI to unlock wallet
		emit requireUnlock();
	}
	// If wallet is still locked, unlock was failed or cancelled, mark context as invalid
	bool valid = getEncryptionStatus() != Locked;

	return UnlockContext(this, valid, was_locked && !fWalletUnlockMintOnly);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *wallet, bool valid, bool relock) :
wallet(wallet),
valid(valid),
relock(relock)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
	if (valid && relock)
	{
		wallet->setWalletLocked(true);
	}
}

void WalletModel::UnlockContext::CopyFrom(const UnlockContext& rhs)
{
	// Transfer context; old object no longer relocks wallet
	*this = rhs;
	rhs.relock = false;
}
