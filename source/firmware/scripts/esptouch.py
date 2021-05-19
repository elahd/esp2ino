import socket
import time

__author__="Jean-Michel Julien"
__version__="0.1.0"

ipBytes = None
ssidBytes = None
bssidBytes = None
passwordBytes = None
data = None
dataToSend = []

addressCount = 0
useBroadcast = False
sendBuffer = bytearray(600)


def getClientSocket():
    global useBroadcast
    sock = socket.socket(socket.AF_INET, # Internet
                 socket.SOCK_DGRAM) # UDP
    if useBroadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    return sock


def sendPacket(_socket, _destination, _size):
    if isinstance(_socket, socket.socket) is not True:
        raise ValueError("sendPacket error invalid socket object")

    global sendBuffer
#    print("{}  Sending {} bytes to {}".format(time.monotonic(), len(sendBuffer[0:_size]), _destination))
    _socket.sendto(sendBuffer[0:_size], _destination)


def getNextTargetAddress():
    global useBroadcast
    global addressCount

    if useBroadcast:
        return ("255.255.255.255", 7001)
    else:
        addressCount += 1
        multicastAddress = "234.{}.{}.{}".format(addressCount, addressCount, addressCount)
        addressCount %= 100
        return (multicastAddress, 7001)


def AddToCRC(b, crc):
    if (b < 0):
        b += 256
    for i in range(8):
        odd = ((b^crc) & 1) == 1
        crc >>= 1
        b >>= 1
        if (odd):
            crc ^= 0x8C # this means crc ^= 140
    return crc


# one data format:(data code should have 2 to 65 data)
# 
#              control byte       high 4 bits    low 4 bits
# 1st 9bits:       0x0             crc(high)      data(high)
# 2nd 9bits:       0x1                sequence header
# 3rd 9bits:       0x0             crc(low)       data(low)
# 
def encodeDataByte(dataByte, sequenceHeader):
    if sequenceHeader > 127 :
        raise ValueError('sequenceHeader must be between 0 and 127')
    # calculate the crc
    crc = 0
    crc = AddToCRC(dataByte, crc)
    crc = AddToCRC(sequenceHeader, crc)

    # split in nibbles
    crc_high, crc_low = crc >> 4, crc & 0x0F
    data_high, data_low = bytes([dataByte])[0] >> 4, bytes([dataByte])[0] & 0x0F

    # reassemble high with high , low with low and add 40
    first = ((crc_high << 4) | data_high) + 40
    # second ninth bit must be set (256 + 40)
    second = 296 + sequenceHeader
    third = ((crc_low << 4) | data_low) + 40

    return (first, second, third)


def getGuideCode():
    return (515, 514, 513, 512)


def getDatumCode():
    global ssidBytes
    global bssidBytes
    global passwordBytes
    global data

    totalDataLength = 5 + len(data)
    passwordLength = len(passwordBytes)
    ssidCrc = 0
    for b in ssidBytes:
        ssidCrc = AddToCRC(b, ssidCrc)
    bssidCrc = 0
    for b in bssidBytes:
        bssidCrc = AddToCRC(b, bssidCrc)

    totalXor = 0
    totalXor ^= totalDataLength
    totalXor ^= passwordLength
    totalXor ^= ssidCrc
    totalXor ^= bssidCrc

    for b in data:
        totalXor ^= b

    return (totalDataLength, passwordLength, ssidCrc, bssidCrc, totalXor)


def getDataCode():
    return (data)


def prepareDataToSend():
    global dataToSend
    global bssidBytes

    # human readable data in the console in pack of three bytes
#    i = 0
#    for b in getDatumCode():
#        print(encodeDataByte(b, i))
#        i += 1
#    
#    iBssid = len(getDatumCode()) + len(getDataCode())
#    bssidLength = len(bssidBytes)
#    indexBssid = 0
#    indexData = 0
#    for b in getDataCode():
#        # add a byte of the bssid every 4 bytes
#        if (indexData % 4) == 0 and indexBssid < bssidLength:
#            print(encodeDataByte(bssidBytes[indexBssid], iBssid))
#            iBssid += 1
#            indexBssid += 1
#        print(encodeDataByte(b, i))
#        i += 1
#        indexData += 1
#    while indexBssid < bssidLength:
#        print(encodeDataByte(bssidBytes[indexBssid], iBssid))
#        iBssid += 1
#        indexBssid += 1

    # The data
    i = 0
    for d in getDatumCode():
        for b in encodeDataByte(d, i):
            dataToSend += [b]
        i += 1

    iBssid = len(getDatumCode()) + len(getDataCode())
    bssidLength = len(bssidBytes)
    indexBssid = 0
    indexData = 0
    for d in getDataCode():
        # add a byte of the bssid every 4 bytes
        if (indexData % 4) == 0 and indexBssid < bssidLength:
            for b in encodeDataByte(bssidBytes[indexBssid], iBssid):
                dataToSend += [b]
            iBssid += 1
            indexBssid += 1
        for b in encodeDataByte(d, i):
            dataToSend += [b]
        i += 1
        indexData += 1
    while indexBssid < bssidLength:
        for b in encodeDataByte(bssidBytes[indexBssid], iBssid):
            dataToSend += [b]
        iBssid += 1
        indexBssid += 1


def sendGuideCode():
    index = 0
    destination = getNextTargetAddress()
    # run for 2 sec send packet every 8 msec
    nexttime = now = time.monotonic()
    endtime = now + 2
    while now < endtime or index != 0:
        now = time.monotonic()
        if now > nexttime:
            sendPacket(getClientSocket(), destination, getGuideCode()[index]) 
            nexttime = now + 0.008
            index += 1
            if index > 3:
                destination = getNextTargetAddress()
            index %= 4


def sendDataCode():
    global dataToSend
    index = 0
    destination = getNextTargetAddress()
    # run for 4 sec send packet every 8 msec
    nexttime = now = time.monotonic()
    endtime = now + 4
    while now < endtime or index != 0:
        now = time.monotonic()
        if now > nexttime:
            sendPacket(getClientSocket(), destination, dataToSend[index]) 
            nexttime = now + 0.008
            index += 1
            if (index % 3) == 0:
                destination = getNextTargetAddress()
            index %= len(dataToSend)


def sendData():
#    print("DATUM: ", getDatumCode())
#    print("GUIDE: ", getGuideCode())
    prepareDataToSend()
    print("Sending data...")
    sendGuideCode()
    sendDataCode()
    print("Done!")


def init(_ssid, _password, _broadcast, _ip, _bssid):
    global ssidBytes
    global ipBytes
    global bssidBytes
    global passwordBytes
    global useBroadcast
    global data

    if _bssid:
        bssidBytes = bytes.fromhex(_bssid)
    else:
        bssidBytes = bytes()

    ssidBytes = bytes(_ssid.encode())
    passwordBytes = bytes(_password.encode())
    ipBytes = bytes(map(int, _ip.split('.')))

    useBroadcast = _broadcast[0] == 'T' or _broadcast[0] == 't'

    if len(ipBytes) != 4:
        raise ValueError("IP address invalid")

    # Data is ip (4 bytes) + password 
    data = ipBytes + passwordBytes

    # + ssid if hidden but this is not enforced on Android..... so we always include it as well
    data += ssidBytes

#    print("DATA length", len(data))
#    print("DATA-->", ":".join("{:02x}".format(c) for c in data))
#    print("bssid-->", ":".join("{:02x}".format(c) for c in bssidBytes))
#    print("ssid-->", ":".join("{:02x}".format(c) for c in ssidBytes))
#    print("Broadcast--> {}".format(useBroadcast))


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 5:
        init(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
        sendData()
        sendData()
        sendData()
    elif len(sys.argv) > 4:
        init(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], None)
        sendData()
        sendData()
        sendData()
    else:
        print("Usage : ESPTouch.py [ssid] [password] [broadcast T/F] [returnIP] [bssid]")
        pass
