using System;
using System.Reactive.Linq;
using System.ComponentModel;
using System.Xml.Serialization;

namespace Bonsai.Reactive
{
    [Description("Delays each event by a random time interval between MinDelay and MaxDelay (seconds).")]
    public class RandomDelay : Combinator
    {
        private readonly Random random = new Random();

        [XmlIgnore]
        [Description("The minimum delay time (seconds).")]
        public TimeSpan MinDelay { get; set; } = TimeSpan.FromSeconds(7);

        [XmlIgnore]
        [Description("The maximum delay time (seconds).")]
        public TimeSpan MaxDelay { get; set; } = TimeSpan.FromSeconds(12);

        [Browsable(false)]
        [XmlElement("MinDelay")]
        public string MinDelayXml
        {
            get => XmlConvert.ToString(MinDelay);
            set => MinDelay = XmlConvert.ToTimeSpan(value);
        }

        [Browsable(false)]
        [XmlElement("MaxDelay")]
        public string MaxDelayXml
        {
            get => XmlConvert.ToString(MaxDelay);
            set => MaxDelay = XmlConvert.ToTimeSpan(value);
        }

        public override IObservable<TSource> Process<TSource>(IObservable<TSource> source)
        {
            return source.SelectMany(value =>
            {
                double randomSeconds = random.NextDouble() * (MaxDelay.TotalSeconds - MinDelay.TotalSeconds) + MinDelay.TotalSeconds;
                TimeSpan randomDelay = TimeSpan.FromSeconds(randomSeconds);
                return Observable.Timer(randomDelay).Select(_ => value);
            });
        }
    }
}
